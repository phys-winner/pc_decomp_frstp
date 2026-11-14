static void baseline_14108DEA6(CPU *c) {
    c->rip=0x14108DEA6ull;
    SET64(c->rcx, rd64((c->rbx + 0xF0ull)));                     /* 00014108DEA6: mov rcx, qword ptr [rbx + 0xf0] */
    flags_logicz(c, c->rcx & c->rcx, 8);                         /* 00014108DEAD: test rcx, rcx */
    if (c->zf) goto L_00014108DEB8;                              /* 00014108DEB0: je 0x14108deb8 */
    SET64(c->rax, rd64((c->rcx)));                               /* 00014108DEB2: mov rax, qword ptr [rcx] */
    { uint64_t _ct = rd64((c->rax + 0x8ull)); push64(c, 0x14108DEB8ull); dispatch(c, _ct); } c->rip = 0x14108DEB8ull; /* 00014108DEB5: call qword ptr [rax + 8] */
L_00014108DEB8:
    c->rip = 0x14108DEB8ull;
    wr64((c->rbx + 0xF0ull), (uint64_t)(c->rdi));                /* 00014108DEB8: mov qword ptr [rbx + 0xf0], rdi */
    flags_logicz(c, c->rdi & c->rdi, 8);                         /* 00014108DEBF: test rdi, rdi */
    if (c->zf) goto L_00014108DECC;                              /* 00014108DEC2: je 0x14108decc */
    SET64(c->rcx, c->rdi);                                       /* 00014108DEC4: mov rcx, rdi */
    push64(c, 0x14108DECCull); dispatch(c, 0x140E1DEA0ull); c->rip = 0x14108DECCull; /* 00014108DEC7: call 0x140e1dea0 */
L_00014108DECC:
    c->rip = 0x14108DECCull;
    wrxm((c->rbx + 0xB0ull), c->xmm[6]);                         /* 00014108DECC: movdqu xmmword ptr [rbx + 0xb0], xmm6 */
    SET64(c->rcx, (c->rbx + 0x20ull));                           /* 00014108DED4: lea rcx, [rbx + 0x20] */
    wrxm((c->rbx + 0xC0ull), c->xmm[7]);                         /* 00014108DED8: movdqu xmmword ptr [rbx + 0xc0], xmm7 */
    push64(c, 0x14108DEE5ull); dispatch(c, 0x14108E7A0ull); c->rip = 0x14108DEE5ull; /* 00014108DEE0: call 0x14108e7a0 */
    dispatch(c, 0x14108DBE5ull); return;                         /* 00014108DEE5: jmp 0x14108dbe5 */
}

static void baseline_14108DEEA(CPU *c) {
    c->rip=0x14108DEEAull;
    SET64(c->rax, rd64((c->r8 + 0xC0ull)));                      /* 00014108DEEA: mov rax, qword ptr [r8 + 0xc0] */
    SET32(c->rcx, rd32((c->r8 + 0xC8ull)));                      /* 00014108DEF1: mov ecx, dword ptr [r8 + 0xc8] */
    SET32(c->rcx, flags_sub(c, R32(c->rcx), 0x10u, 4));          /* 00014108DEF8: sub ecx, 0x10 */
    c->xmm[1] = rdxm((c->rax));                                  /* 00014108DEFB: movups xmm1, xmmword ptr [rax] */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108DEFE: mov dword ptr [r8 + 0xc8], ecx */
    SET64(c->rdx, (c->rax + 0x10ull));                           /* 00014108DF05: lea rdx, [rax + 0x10] */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108DF09: mov qword ptr [r8 + 0xc0], rdx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFFCu, 4));    /* 00014108DF10: add ecx, -4 */
    SET32(c->rax, rd32((c->rdx)));                               /* 00014108DF13: mov eax, dword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x4ull, 8));              /* 00014108DF15: add rdx, 4 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108DF19: mov qword ptr [r8 + 0xc0], rdx */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108DF20: mov dword ptr [r8 + 0xc8], ecx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFFCu, 4));    /* 00014108DF27: add ecx, -4 */
    wr32((c->rbp + 0x28ull), (uint32_t)(R32(c->rax)));           /* 00014108DF2A: mov dword ptr [rbp + 0x28], eax */
    SET32(c->rax, rd32((c->rdx)));                               /* 00014108DF2D: mov eax, dword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x4ull, 8));              /* 00014108DF2F: add rdx, 4 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108DF33: mov qword ptr [r8 + 0xc0], rdx */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108DF3A: mov dword ptr [r8 + 0xc8], ecx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFFCu, 4));    /* 00014108DF41: add ecx, -4 */
    wr32((c->rbp + 0x38ull), (uint32_t)(R32(c->rax)));           /* 00014108DF44: mov dword ptr [rbp + 0x38], eax */
    SET32(c->rax, rd32((c->rdx)));                               /* 00014108DF47: mov eax, dword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x4ull, 8));              /* 00014108DF49: add rdx, 4 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108DF4D: mov qword ptr [r8 + 0xc0], rdx */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108DF54: mov dword ptr [r8 + 0xc8], ecx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFFCu, 4));    /* 00014108DF5B: add ecx, -4 */
    wr32((c->rbp + (int64_t)(-40)), (uint32_t)(R32(c->rax)));    /* 00014108DF5E: mov dword ptr [rbp - 0x28], eax */
    SET32(c->rax, rd32((c->rdx)));                               /* 00014108DF61: mov eax, dword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x4ull, 8));              /* 00014108DF63: add rdx, 4 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108DF67: mov qword ptr [r8 + 0xc0], rdx */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108DF6E: mov dword ptr [r8 + 0xc8], ecx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFF8u, 4));    /* 00014108DF75: add ecx, -8 */
    wr32((c->rbp + (int64_t)(-44)), (uint32_t)(R32(c->rax)));    /* 00014108DF78: mov dword ptr [rbp - 0x2c], eax */
    SET64(c->rdi, rd64((c->rdx)));                               /* 00014108DF7B: mov rdi, qword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x8ull, 8));              /* 00014108DF7E: add rdx, 8 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108DF82: mov qword ptr [r8 + 0xc0], rdx */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108DF89: mov dword ptr [r8 + 0xc8], ecx */
    c->xmm[6] = rdxm((c->rdx));                                  /* 00014108DF90: movups xmm6, xmmword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x10ull, 8));             /* 00014108DF93: add rdx, 0x10 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108DF97: mov qword ptr [r8 + 0xc0], rdx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFF0u, 4));    /* 00014108DF9E: add ecx, -0x10 */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108DFA1: mov dword ptr [r8 + 0xc8], ecx */
    c->xmm[0] = rdxm((c->rdx));                                  /* 00014108DFA8: movups xmm0, xmmword ptr [rdx] */
    SET64(c->rax, (c->rdx + 0x10ull));                           /* 00014108DFAB: lea rax, [rdx + 0x10] */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rax));                 /* 00014108DFAF: mov qword ptr [r8 + 0xc0], rax */
    SET32(c->rax, (c->rcx + (int64_t)(-16)));                    /* 00014108DFB6: lea eax, [rcx - 0x10] */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rax)));            /* 00014108DFB9: mov dword ptr [r8 + 0xc8], eax */
    wrxm((c->rbx + 0x70ull), c->xmm[1]);                         /* 00014108DFC0: movdqu xmmword ptr [rbx + 0x70], xmm1 */
    wrxm((c->rbx + 0x60ull), c->xmm[1]);                         /* 00014108DFC5: movdqu xmmword ptr [rbx + 0x60], xmm1 */
    sse_load_ss(&c->xmm[1], rdss((c->rbp + 0x38ull)));           /* 00014108DFCA: movss xmm1, dword ptr [rbp + 0x38] */
    { XMM _s = c->xmm[0]; c->xmm[7].f32[0] = sse_rcp(_s.f32[0]); c->xmm[7].f32[1] = sse_rcp(_s.f32[1]); c->xmm[7].f32[2] = sse_rcp(_s.f32[2]); c->xmm[7].f32[3] = sse_rcp(_s.f32[3]); } /* 00014108DFCF: rcpps xmm7, xmm0 */
    wr32((c->rbx + 0xE8ull), (uint32_t)(0x2u));                  /* 00014108DFD2: mov dword ptr [rbx + 0xe8], 2 */
    sse_load_ss(&c->xmm[0], rdss((c->rbp + 0x28ull)));           /* 00014108DFDC: movss xmm0, dword ptr [rbp + 0x28] */
    wrss((c->rbx + 0xD4ull), c->xmm[1].f32[0]);                  /* 00014108DFE1: movss dword ptr [rbx + 0xd4], xmm1 */
    sse_load_ss(&c->xmm[1], rdss((c->rbp + (int64_t)(-44))));    /* 00014108DFE9: movss xmm1, dword ptr [rbp - 0x2c] */
    wrss((c->rbx + 0xD0ull), c->xmm[0].f32[0]);                  /* 00014108DFEE: movss dword ptr [rbx + 0xd0], xmm0 */
    sse_load_ss(&c->xmm[0], rdss((c->rbp + (int64_t)(-40))));    /* 00014108DFF6: movss xmm0, dword ptr [rbp - 0x28] */
    wrss((c->rbx + 0xE4ull), c->xmm[1].f32[0]);                  /* 00014108DFFB: movss dword ptr [rbx + 0xe4], xmm1 */
    wrss((c->rbx + 0xD8ull), c->xmm[0].f32[0]);                  /* 00014108E003: movss dword ptr [rbx + 0xd8], xmm0 */
    { dispatch(c,0x00014108DEA6ull); return; }                                         /* 00014108E00B: jmp 0x14108dea6 */
}

static void baseline_14108E010(CPU *c) {
    c->rip=0x14108E010ull;
    SET64(c->rax, rd64((c->r8 + 0xC0ull)));                      /* 00014108E010: mov rax, qword ptr [r8 + 0xc0] */
    SET32(c->rdx, rd32((c->r9 + 0xC8ull)));                      /* 00014108E017: mov edx, dword ptr [r9 + 0xc8] */
    SET32(c->rdx, flags_sub(c, R32(c->rdx), 0x10u, 4));          /* 00014108E01E: sub edx, 0x10 */
    c->xmm[1] = rdxm((c->rax));                                  /* 00014108E021: movups xmm1, xmmword ptr [rax] */
    wr32((c->r9 + 0xC8ull), (uint32_t)(R32(c->rdx)));            /* 00014108E024: mov dword ptr [r9 + 0xc8], edx */
    SET64(c->r8, (c->rax + 0x10ull));                            /* 00014108E02B: lea r8, [rax + 0x10] */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->r8));                  /* 00014108E02F: mov qword ptr [r9 + 0xc0], r8 */
    SET32(c->rdx, flags_add(c, R32(c->rdx), 0xFFFFFFFCu, 4));    /* 00014108E036: add edx, -4 */
    SET32(c->rax, rd32((c->r8)));                                /* 00014108E039: mov eax, dword ptr [r8] */
    SET64(c->r8, flags_add(c, c->r8, 0x4ull, 8));                /* 00014108E03C: add r8, 4 */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->r8));                  /* 00014108E040: mov qword ptr [r9 + 0xc0], r8 */
    wr32((c->r9 + 0xC8ull), (uint32_t)(R32(c->rdx)));            /* 00014108E047: mov dword ptr [r9 + 0xc8], edx */
    SET32(c->rdx, flags_add(c, R32(c->rdx), 0xFFFFFFFCu, 4));    /* 00014108E04E: add edx, -4 */
    wr32((c->rbp + 0x28ull), (uint32_t)(R32(c->rax)));           /* 00014108E051: mov dword ptr [rbp + 0x28], eax */
    SET32(c->rax, rd32((c->r8)));                                /* 00014108E054: mov eax, dword ptr [r8] */
    SET64(c->r8, flags_add(c, c->r8, 0x4ull, 8));                /* 00014108E057: add r8, 4 */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->r8));                  /* 00014108E05B: mov qword ptr [r9 + 0xc0], r8 */
    wr32((c->r9 + 0xC8ull), (uint32_t)(R32(c->rdx)));            /* 00014108E062: mov dword ptr [r9 + 0xc8], edx */
    SET32(c->rdx, flags_decs(c, R32(c->rdx), 4));                /* 00014108E069: dec edx */
    wr32((c->rbp + 0x38ull), (uint32_t)(R32(c->rax)));           /* 00014108E06B: mov dword ptr [rbp + 0x38], eax */
    SET32(c->rcx, (rd8((c->r8))));                               /* 00014108E06E: movzx ecx, byte ptr [r8] */
    SET64(c->r8, flags_incs(c, c->r8, 8));                       /* 00014108E072: inc r8 */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->r8));                  /* 00014108E075: mov qword ptr [r9 + 0xc0], r8 */
    wr32((c->r9 + 0xC8ull), (uint32_t)(R32(c->rdx)));            /* 00014108E07C: mov dword ptr [r9 + 0xc8], edx */
    SET32(c->rdx, flags_add(c, R32(c->rdx), 0xFFFFFFF8u, 4));    /* 00014108E083: add edx, -8 */
    SET64(c->rdi, rd64((c->r8)));                                /* 00014108E086: mov rdi, qword ptr [r8] */
    SET64(c->r8, flags_add(c, c->r8, 0x8ull, 8));                /* 00014108E089: add r8, 8 */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->r8));                  /* 00014108E08D: mov qword ptr [r9 + 0xc0], r8 */
    wr32((c->r9 + 0xC8ull), (uint32_t)(R32(c->rdx)));            /* 00014108E094: mov dword ptr [r9 + 0xc8], edx */
    SET32(c->rdx, flags_add(c, R32(c->rdx), 0xFFFFFFF0u, 4));    /* 00014108E09B: add edx, -0x10 */
    c->xmm[6] = rdxm((c->r8));                                   /* 00014108E09E: movups xmm6, xmmword ptr [r8] */
    wr32((c->r9 + 0xC8ull), (uint32_t)(R32(c->rdx)));            /* 00014108E0A2: mov dword ptr [r9 + 0xc8], edx */
    SET64(c->r8, flags_add(c, c->r8, 0x10ull, 8));               /* 00014108E0A9: add r8, 0x10 */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->r8));                  /* 00014108E0AD: mov qword ptr [r9 + 0xc0], r8 */
    SET32(c->rdx, flags_add(c, R32(c->rdx), 0xFFFFFFF0u, 4));    /* 00014108E0B4: add edx, -0x10 */
    c->xmm[0] = rdxm((c->r8));                                   /* 00014108E0B7: movups xmm0, xmmword ptr [r8] */
    wr32((c->r9 + 0xC8ull), (uint32_t)(R32(c->rdx)));            /* 00014108E0BB: mov dword ptr [r9 + 0xc8], edx */
    SET64(c->r8, flags_add(c, c->r8, 0x10ull, 8));               /* 00014108E0C2: add r8, 0x10 */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->r8));                  /* 00014108E0C6: mov qword ptr [r9 + 0xc0], r8 */
    SET32(c->rdx, flags_add(c, R32(c->rdx), 0xFFFFFFFCu, 4));    /* 00014108E0CD: add edx, -4 */
    { XMM _s = c->xmm[0]; c->xmm[7].f32[0] = sse_rcp(_s.f32[0]); c->xmm[7].f32[1] = sse_rcp(_s.f32[1]); c->xmm[7].f32[2] = sse_rcp(_s.f32[2]); c->xmm[7].f32[3] = sse_rcp(_s.f32[3]); } /* 00014108E0D0: rcpps xmm7, xmm0 */
    SET32(c->rax, rd32((c->r8)));                                /* 00014108E0D3: mov eax, dword ptr [r8] */
    SET64(c->r8, flags_add(c, c->r8, 0x4ull, 8));                /* 00014108E0D6: add r8, 4 */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->r8));                  /* 00014108E0DA: mov qword ptr [r9 + 0xc0], r8 */
    wr32((c->r9 + 0xC8ull), (uint32_t)(R32(c->rdx)));            /* 00014108E0E1: mov dword ptr [r9 + 0xc8], edx */
    SET32(c->rdx, flags_add(c, R32(c->rdx), 0xFFFFFFFCu, 4));    /* 00014108E0E8: add edx, -4 */
    wr32((c->rbp + (int64_t)(-40)), (uint32_t)(R32(c->rax)));    /* 00014108E0EB: mov dword ptr [rbp - 0x28], eax */
    SET32(c->rax, rd32((c->r8)));                                /* 00014108E0EE: mov eax, dword ptr [r8] */
    SET64(c->r8, flags_add(c, c->r8, 0x4ull, 8));                /* 00014108E0F1: add r8, 4 */
    sse_load_ss(&c->xmm[0], rdss((c->rbp + 0x28ull)));           /* 00014108E0F5: movss xmm0, dword ptr [rbp + 0x28] */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->r8));                  /* 00014108E0FA: mov qword ptr [r9 + 0xc0], r8 */
    wr32((c->r9 + 0xC8ull), (uint32_t)(R32(c->rdx)));            /* 00014108E101: mov dword ptr [r9 + 0xc8], edx */
    wr32((c->rbp + (int64_t)(-48)), (uint32_t)(R32(c->rax)));    /* 00014108E108: mov dword ptr [rbp - 0x30], eax */
    SET32(c->rax, rd32((c->r8)));                                /* 00014108E10B: mov eax, dword ptr [r8] */
    wr32((c->rbp + (int64_t)(-44)), (uint32_t)(R32(c->rax)));    /* 00014108E10E: mov dword ptr [rbp - 0x2c], eax */
    SET64(c->rax, (c->r8 + 0x4ull));                             /* 00014108E111: lea rax, [r8 + 4] */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->rax));                 /* 00014108E115: mov qword ptr [r9 + 0xc0], rax */
    SET32(c->rax, (c->rdx + (int64_t)(-4)));                     /* 00014108E11C: lea eax, [rdx - 4] */
    wr32((c->r9 + 0xC8ull), (uint32_t)(R32(c->rax)));            /* 00014108E11F: mov dword ptr [r9 + 0xc8], eax */
    wrxm((c->rbx + 0x70ull), c->xmm[1]);                         /* 00014108E126: movdqu xmmword ptr [rbx + 0x70], xmm1 */
    wrxm((c->rbx + 0x60ull), c->xmm[1]);                         /* 00014108E12B: movdqu xmmword ptr [rbx + 0x60], xmm1 */
    sse_load_ss(&c->xmm[1], rdss((c->rbp + 0x38ull)));           /* 00014108E130: movss xmm1, dword ptr [rbp + 0x38] */
    wrss((c->rbx + 0x108ull), c->xmm[1].f32[0]);                 /* 00014108E135: movss dword ptr [rbx + 0x108], xmm1 */
    wrss((c->rbx + 0x104ull), c->xmm[0].f32[0]);                 /* 00014108E13D: movss dword ptr [rbx + 0x104], xmm0 */
    wr32((c->rbx + 0xE8ull), (uint32_t)(0x4u));                  /* 00014108E145: mov dword ptr [rbx + 0xe8], 4 */
    wr8((c->rbx + 0x11Aull), (uint8_t)(R8L(c->rcx)));            /* 00014108E14F: mov byte ptr [rbx + 0x11a], cl */
    SET64(c->rcx, rd64((c->rbx + 0xF0ull)));                     /* 00014108E155: mov rcx, qword ptr [rbx + 0xf0] */
    flags_logicz(c, c->rcx & c->rcx, 8);                         /* 00014108E15C: test rcx, rcx */
    if (c->zf) goto L_00014108E167;                              /* 00014108E15F: je 0x14108e167 */
    SET64(c->rax, rd64((c->rcx)));                               /* 00014108E161: mov rax, qword ptr [rcx] */
    { uint64_t _ct = rd64((c->rax + 0x8ull)); push64(c, 0x14108E167ull); dispatch(c, _ct); } c->rip = 0x14108E167ull; /* 00014108E164: call qword ptr [rax + 8] */
L_00014108E167:
    c->rip = 0x14108E167ull;
    wr64((c->rbx + 0xF0ull), (uint64_t)(c->rdi));                /* 00014108E167: mov qword ptr [rbx + 0xf0], rdi */
    flags_logicz(c, c->rdi & c->rdi, 8);                         /* 00014108E16E: test rdi, rdi */
    if (c->zf) goto L_00014108E17B;                              /* 00014108E171: je 0x14108e17b */
    SET64(c->rcx, c->rdi);                                       /* 00014108E173: mov rcx, rdi */
    push64(c, 0x14108E17Bull); dispatch(c, 0x140E1DEA0ull); c->rip = 0x14108E17Bull; /* 00014108E176: call 0x140e1dea0 */
L_00014108E17B:
    c->rip = 0x14108E17Bull;
    sse_load_ss(&c->xmm[0], rdss((c->rbp + (int64_t)(-40))));    /* 00014108E17B: movss xmm0, dword ptr [rbp - 0x28] */
    sse_load_ss(&c->xmm[1], rdss((c->rbp + (int64_t)(-44))));    /* 00014108E180: movss xmm1, dword ptr [rbp - 0x2c] */
    wrss((c->rbx + 0x10Cull), c->xmm[0].f32[0]);                 /* 00014108E185: movss dword ptr [rbx + 0x10c], xmm0 */
    sse_load_ss(&c->xmm[0], rdss((c->rbp + (int64_t)(-48))));    /* 00014108E18D: movss xmm0, dword ptr [rbp - 0x30] */
    wrss((c->rbx + 0x114ull), c->xmm[0].f32[0]);                 /* 00014108E192: movss dword ptr [rbx + 0x114], xmm0 */
    wrss((c->rbx + 0x110ull), c->xmm[1].f32[0]);                 /* 00014108E19A: movss dword ptr [rbx + 0x110], xmm1 */
    wrxm((c->rbx + 0xB0ull), c->xmm[6]);                         /* 00014108E1A2: movdqu xmmword ptr [rbx + 0xb0], xmm6 */
    wrxm((c->rbx + 0xC0ull), c->xmm[7]);                         /* 00014108E1AA: movdqu xmmword ptr [rbx + 0xc0], xmm7 */
    dispatch(c, 0x14108DBE5ull); return;                         /* 00014108E1B2: jmp 0x14108dbe5 */
}

static void baseline_14108E1B7(CPU *c) {
    c->rip=0x14108E1B7ull;
    SET64(c->rax, rd64((c->r8 + 0xC0ull)));                      /* 00014108E1B7: mov rax, qword ptr [r8 + 0xc0] */
    SET32(c->rcx, rd32((c->r8 + 0xC8ull)));                      /* 00014108E1BE: mov ecx, dword ptr [r8 + 0xc8] */
    SET32(c->rcx, flags_sub(c, R32(c->rcx), 0x10u, 4));          /* 00014108E1C5: sub ecx, 0x10 */
    c->xmm[1] = rdxm((c->rax));                                  /* 00014108E1C8: movups xmm1, xmmword ptr [rax] */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108E1CB: mov dword ptr [r8 + 0xc8], ecx */
    SET64(c->rdx, (c->rax + 0x10ull));                           /* 00014108E1D2: lea rdx, [rax + 0x10] */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108E1D6: mov qword ptr [r8 + 0xc0], rdx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFFCu, 4));    /* 00014108E1DD: add ecx, -4 */
    SET32(c->rax, rd32((c->rdx)));                               /* 00014108E1E0: mov eax, dword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x4ull, 8));              /* 00014108E1E2: add rdx, 4 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108E1E6: mov qword ptr [r8 + 0xc0], rdx */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108E1ED: mov dword ptr [r8 + 0xc8], ecx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFFCu, 4));    /* 00014108E1F4: add ecx, -4 */
    wr32((c->rbp + 0x28ull), (uint32_t)(R32(c->rax)));           /* 00014108E1F7: mov dword ptr [rbp + 0x28], eax */
    SET32(c->rax, rd32((c->rdx)));                               /* 00014108E1FA: mov eax, dword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x4ull, 8));              /* 00014108E1FC: add rdx, 4 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108E200: mov qword ptr [r8 + 0xc0], rdx */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108E207: mov dword ptr [r8 + 0xc8], ecx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFFCu, 4));    /* 00014108E20E: add ecx, -4 */
    wr32((c->rbp + 0x38ull), (uint32_t)(R32(c->rax)));           /* 00014108E211: mov dword ptr [rbp + 0x38], eax */
    SET32(c->rax, rd32((c->rdx)));                               /* 00014108E214: mov eax, dword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x4ull, 8));              /* 00014108E216: add rdx, 4 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108E21A: mov qword ptr [r8 + 0xc0], rdx */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108E221: mov dword ptr [r8 + 0xc8], ecx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFFCu, 4));    /* 00014108E228: add ecx, -4 */
    wr32((c->rbp + (int64_t)(-40)), (uint32_t)(R32(c->rax)));    /* 00014108E22B: mov dword ptr [rbp - 0x28], eax */
    SET32(c->rax, rd32((c->rdx)));                               /* 00014108E22E: mov eax, dword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x4ull, 8));              /* 00014108E230: add rdx, 4 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108E234: mov qword ptr [r8 + 0xc0], rdx */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108E23B: mov dword ptr [r8 + 0xc8], ecx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFF8u, 4));    /* 00014108E242: add ecx, -8 */
    wr32((c->rbp + (int64_t)(-44)), (uint32_t)(R32(c->rax)));    /* 00014108E245: mov dword ptr [rbp - 0x2c], eax */
    SET64(c->rdi, rd64((c->rdx)));                               /* 00014108E248: mov rdi, qword ptr [rdx] */
    SET64(c->rdx, flags_add(c, c->rdx, 0x8ull, 8));              /* 00014108E24B: add rdx, 8 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108E24F: mov qword ptr [r8 + 0xc0], rdx */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108E256: mov dword ptr [r8 + 0xc8], ecx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFF0u, 4));    /* 00014108E25D: add ecx, -0x10 */
    c->xmm[6] = rdxm((c->rdx));                                  /* 00014108E260: movups xmm6, xmmword ptr [rdx] */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108E263: mov dword ptr [r8 + 0xc8], ecx */
    SET64(c->rdx, flags_add(c, c->rdx, 0x10ull, 8));             /* 00014108E26A: add rdx, 0x10 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108E26E: mov qword ptr [r8 + 0xc0], rdx */
    SET32(c->rcx, flags_add(c, R32(c->rcx), 0xFFFFFFF0u, 4));    /* 00014108E275: add ecx, -0x10 */
    c->xmm[0] = rdxm((c->rdx));                                  /* 00014108E278: movups xmm0, xmmword ptr [rdx] */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rcx)));            /* 00014108E27B: mov dword ptr [r8 + 0xc8], ecx */
    SET64(c->rdx, flags_add(c, c->rdx, 0x10ull, 8));             /* 00014108E282: add rdx, 0x10 */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rdx));                 /* 00014108E286: mov qword ptr [r8 + 0xc0], rdx */
    { XMM _s = c->xmm[0]; c->xmm[7].f32[0] = sse_rcp(_s.f32[0]); c->xmm[7].f32[1] = sse_rcp(_s.f32[1]); c->xmm[7].f32[2] = sse_rcp(_s.f32[2]); c->xmm[7].f32[3] = sse_rcp(_s.f32[3]); } /* 00014108E28D: rcpps xmm7, xmm0 */
    SET32(c->rax, rd32((c->rdx)));                               /* 00014108E290: mov eax, dword ptr [rdx] */
    wr32((c->rbp + (int64_t)(-48)), (uint32_t)(R32(c->rax)));    /* 00014108E292: mov dword ptr [rbp - 0x30], eax */
    SET64(c->rax, (c->rdx + 0x4ull));                            /* 00014108E295: lea rax, [rdx + 4] */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rax));                 /* 00014108E299: mov qword ptr [r8 + 0xc0], rax */
    SET32(c->rax, (c->rcx + (int64_t)(-4)));                     /* 00014108E2A0: lea eax, [rcx - 4] */
    wr32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rax)));            /* 00014108E2A3: mov dword ptr [r8 + 0xc8], eax */
    wr32((c->rbx + 0xE8ull), (uint32_t)(0x5u));                  /* 00014108E2AA: mov dword ptr [rbx + 0xe8], 5 */
    sse_load_ss(&c->xmm[0], rdss((c->rbp + 0x28ull)));           /* 00014108E2B4: movss xmm0, dword ptr [rbp + 0x28] */
    wrxm((c->rbx + 0x70ull), c->xmm[1]);                         /* 00014108E2B9: movdqu xmmword ptr [rbx + 0x70], xmm1 */
    wrxm((c->rbx + 0x60ull), c->xmm[1]);                         /* 00014108E2BE: movdqu xmmword ptr [rbx + 0x60], xmm1 */
    sse_load_ss(&c->xmm[1], rdss((c->rbp + 0x38ull)));           /* 00014108E2C3: movss xmm1, dword ptr [rbp + 0x38] */
    wrss((c->rbx + 0x108ull), c->xmm[1].f32[0]);                 /* 00014108E2C8: movss dword ptr [rbx + 0x108], xmm1 */
    sse_load_ss(&c->xmm[1], rdss((c->rbp + (int64_t)(-44))));    /* 00014108E2D0: movss xmm1, dword ptr [rbp - 0x2c] */
    wrss((c->rbx + 0x104ull), c->xmm[0].f32[0]);                 /* 00014108E2D5: movss dword ptr [rbx + 0x104], xmm0 */
    sse_load_ss(&c->xmm[0], rdss((c->rbp + (int64_t)(-40))));    /* 00014108E2DD: movss xmm0, dword ptr [rbp - 0x28] */
    wrss((c->rbx + 0x114ull), c->xmm[1].f32[0]);                 /* 00014108E2E2: movss dword ptr [rbx + 0x114], xmm1 */
    wrss((c->rbx + 0x110ull), c->xmm[0].f32[0]);                 /* 00014108E2EA: movss dword ptr [rbx + 0x110], xmm0 */
    SET64(c->rcx, rd64((c->rbx + 0xF0ull)));                     /* 00014108E2F2: mov rcx, qword ptr [rbx + 0xf0] */
    flags_logicz(c, c->rcx & c->rcx, 8);                         /* 00014108E2F9: test rcx, rcx */
    if (c->zf) goto L_00014108E304;                              /* 00014108E2FC: je 0x14108e304 */
    SET64(c->rax, rd64((c->rcx)));                               /* 00014108E2FE: mov rax, qword ptr [rcx] */
    { uint64_t _ct = rd64((c->rax + 0x8ull)); push64(c, 0x14108E304ull); dispatch(c, _ct); } c->rip = 0x14108E304ull; /* 00014108E301: call qword ptr [rax + 8] */
L_00014108E304:
    c->rip = 0x14108E304ull;
    wr64((c->rbx + 0xF0ull), (uint64_t)(c->rdi));                /* 00014108E304: mov qword ptr [rbx + 0xf0], rdi */
    flags_logicz(c, c->rdi & c->rdi, 8);                         /* 00014108E30B: test rdi, rdi */
    if (c->zf) goto L_00014108E318;                              /* 00014108E30E: je 0x14108e318 */
    SET64(c->rcx, c->rdi);                                       /* 00014108E310: mov rcx, rdi */
    push64(c, 0x14108E318ull); dispatch(c, 0x140E1DEA0ull); c->rip = 0x14108E318ull; /* 00014108E313: call 0x140e1dea0 */
L_00014108E318:
    c->rip = 0x14108E318ull;
    sse_load_ss(&c->xmm[0], rdss((c->rbp + (int64_t)(-48))));    /* 00014108E318: movss xmm0, dword ptr [rbp - 0x30] */
    wrss((c->rbx + 0x10Cull), c->xmm[0].f32[0]);                 /* 00014108E31D: movss dword ptr [rbx + 0x10c], xmm0 */
    wrxm((c->rbx + 0xB0ull), c->xmm[6]);                         /* 00014108E325: movdqu xmmword ptr [rbx + 0xb0], xmm6 */
    wrxm((c->rbx + 0xC0ull), c->xmm[7]);                         /* 00014108E32D: movdqu xmmword ptr [rbx + 0xc0], xmm7 */
    dispatch(c, 0x14108DBE5ull); return;                         /* 00014108E335: jmp 0x14108dbe5 */
}
