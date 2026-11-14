/* Exact separate static lift of the 12 approved RET-adjacent leaves. */

#include "switch_aot_bodies.inc"

#include "switch_extension_bodies.inc"

static void ret_leaf_140E55B70(CPU *c)
{
    c->rip = 0x140E55B70ull;
    dispatch(c, 0x140E56170ull); return;                         /* 000140E55B70: jmp 0x140e56170 */
}

static void ret_leaf_140FEE310(CPU *c)
{
    c->rip = 0x140FEE310ull;
    SET64(c->rcx, flags_add(c, c->rcx, 0x18ull, 8));             /* 000140FEE310: add rcx, 0x18 */
    dispatch(c, 0x140E52950ull); return;                         /* 000140FEE314: jmp 0x140e52950 */
}

static void ret_leaf_141862610(CPU *c)
{
    c->rip = 0x141862610ull;
    dispatch(c, 0x141633110ull); return;                         /* 000141862610: jmp 0x141633110 */
}

static void ret_leaf_141A868E0(CPU *c)
{
    c->rip = 0x141A868E0ull;
    wr8((c->rcx + 0xE0ull), (uint8_t)(0x1u));                    /* 000141A868E0: mov byte ptr [rcx + 0xe0], 1 */
    SET32(c->r8, flags_logicz(c, R32(c->r8) ^ R32(c->r8), 4));   /* 000141A868E7: xor r8d, r8d */
    SET64(c->rcx, rd64(GVA(0x143FFA818)));                       /* 000141A868EA: mov rcx, qword ptr [rip + 0x2573f27] */
    SET32(c->rdx, (c->r8 + 0x44ull));                            /* 000141A868F1: lea edx, [r8 + 0x44] */
    SET64(c->rcx, rd64((c->rcx)));                               /* 000141A868F5: mov rcx, qword ptr [rcx] */
    dispatch(c, 0x1417FCA30ull); return;                         /* 000141A868F8: jmp 0x1417fca30 */
}

static void ret_leaf_141CA5810(CPU *c)
{
    c->rip = 0x141CA5810ull;
    SET64(c->rcx, GVA(0x142A89868));                             /* 000141CA5810: lea rcx, [rip + 0xde4051] */
    dispatch(c, 0x140EDFE30ull); return;                         /* 000141CA5817: jmp 0x140edfe30 */
}

static void ret_leaf_141CA59B0(CPU *c)
{
    c->rip = 0x141CA59B0ull;
    SET64(c->rcx, GVA(0x142A89968));                             /* 000141CA59B0: lea rcx, [rip + 0xde3fb1] */
    dispatch(c, 0x14100F750ull); return;                         /* 000141CA59B7: jmp 0x14100f750 */
}

static void ret_leaf_141CA5B70(CPU *c)
{
    c->rip = 0x141CA5B70ull;
    SET64(c->rcx, GVA(0x142A8A058));                             /* 000141CA5B70: lea rcx, [rip + 0xde44e1] */
    dispatch(c, 0x140E0D870ull); return;                         /* 000141CA5B77: jmp 0x140e0d870 */
}

static void ret_leaf_141CAB7B0(CPU *c)
{
    c->rip = 0x141CAB7B0ull;
    SET64(c->rcx, GVA(0x143FBEED0));                             /* 000141CAB7B0: lea rcx, [rip + 0x2313719] */
    dispatch(c, 0x140E44E10ull); return;                         /* 000141CAB7B7: jmp 0x140e44e10 */
}

static void ret_leaf_141CAC080(CPU *c)
{
    c->rip = 0x141CAC080ull;
    SET32(c->rdx, 0x8u);                                         /* 000141CAC080: mov edx, 8 */
    SET64(c->r9, GVA(0x140E52950));                              /* 000141CAC085: lea r9, [rip - 0xe5973c] */
    SET64(c->rcx, GVA(0x143FC0870));                             /* 000141CAC08C: lea rcx, [rip + 0x23147dd] */
    SET32(c->r8, (c->rdx + (int64_t)(-6)));                      /* 000141CAC093: lea r8d, [rdx - 6] */
    dispatch(c, 0x141AE0E54ull); return;                         /* 000141CAC097: jmp 0x141ae0e54 */
}

static void ret_leaf_141CB9AA0(CPU *c)
{
    c->rip = 0x141CB9AA0ull;
    SET64(c->rcx, GVA(0x143FF1E50));                             /* 000141CB9AA0: lea rcx, [rip + 0x23383a9] */
    dispatch(c, 0x140E51700ull); return;                         /* 000141CB9AA7: jmp 0x140e51700 */
}

static void ret_leaf_141CBAC50(CPU *c)
{
    c->rip = 0x141CBAC50ull;
    SET64(c->rcx, GVA(0x143FF5F38));                             /* 000141CBAC50: lea rcx, [rip + 0x233b2e1] */
    dispatch(c, 0x140D99010ull); return;                         /* 000141CBAC57: jmp 0x140d99010 */
}

static void ret_leaf_141CBDAE0(CPU *c)
{
    c->rip = 0x141CBDAE0ull;
    SET64(c->rcx, GVA(0x143FFA7D8));                             /* 000141CBDAE0: lea rcx, [rip + 0x233ccf1] */
    dispatch(c, 0x140E52950ull); return;                         /* 000141CBDAE7: jmp 0x140e52950 */
}

static void callback_leaf_141947E84(CPU *c)
{
    c->rip = 0x141947E84ull;
    uint64_t _ind = 0;
L_000141947E84:
    c->rip = 0x141947E84ull;
    SET64(c->rax, rd64((c->rcx)));                               /* 000141947E84: mov rax, qword ptr [rcx] */
L_000141947E87:
    c->rip = 0x141947E87ull;
    { _ind = rd64((c->rax + 0x108ull)); goto _ljump; }           /* 000141947E87: jmp qword ptr [rax + 0x108] */
  _ljump:
    switch (_ind) {
      case 0x141947E84ull: goto L_000141947E84;
      case 0x141947E87ull: goto L_000141947E87;
      default: dispatch_jmp(c, _ind); return;
    }
}

static void scene_switch_14108DEA6(CPU *c)
{
    c->rip = 0x14108DEA6ull;
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

static void scene_switch_14108DEEA(CPU *c)
{
    c->rip = 0x14108DEEAull;
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
    dispatch(c, 0x14108DEA6ull); return;                         /* 00014108E00B: jmp 0x14108dea6 */
}

static void scene_switch_14108E010(CPU *c)
{
    c->rip = 0x14108E010ull;
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

static void scene_switch_14108E1B7(CPU *c)
{
    c->rip = 0x14108E1B7ull;
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

static void compare_switch_140DBD134(CPU *c)
{
    c->rip = 0x140DBD134ull;
    sse_compare(c, c->xmm[0].f32[0], c->xmm[6].f32[0]);          /* 000140DBD134: comiss xmm0, xmm6 */
    SET8L(c->rax, ((!c->cf) ? 1 : 0));                           /* 000140DBD137: setae al */
    dispatch(c, 0x140DBD117ull); return;                         /* 000140DBD13A: jmp 0x140dbd117 */
}

static void compare_switch_140DBD13C(CPU *c)
{
    c->rip = 0x140DBD13Cull;
    sse_compare(c, c->xmm[0].f32[0], c->xmm[6].f32[0]);          /* 000140DBD13C: comiss xmm0, xmm6 */
    dispatch(c, 0x140DBD114ull); return;                         /* 000140DBD13F: jmp 0x140dbd114 */
}

static void bounded_switch_140DBCE94(CPU *c)
{
    c->rip = 0x140DBCE94ull;
    flags_sub(c, R32(c->rax), R32(c->rbx), 4);                   /* 000140DBCE94: cmp eax, ebx */
    SET8L(c->rax, (((c->sf != c->of)) ? 1 : 0));                 /* 000140DBCE96: setl al */
    dispatch(c, 0x140DBCE78ull); return;                         /* 000140DBCE99: jmp 0x140dbce78 */
}

static void bounded_switch_140DBCE9B(CPU *c)
{
    c->rip = 0x140DBCE9Bull;
    flags_sub(c, R32(c->rax), R32(c->rbx), 4);                   /* 000140DBCE9B: cmp eax, ebx */
    SET8L(c->rax, (((c->zf || (c->sf != c->of))) ? 1 : 0));      /* 000140DBCE9D: setle al */
    dispatch(c, 0x140DBCE78ull); return;                         /* 000140DBCEA0: jmp 0x140dbce78 */
}

static void bounded_switch_140DBCEA2(CPU *c)
{
    c->rip = 0x140DBCEA2ull;
    flags_sub(c, R32(c->rax), R32(c->rbx), 4);                   /* 000140DBCEA2: cmp eax, ebx */
    SET8L(c->rax, (((c->sf == c->of)) ? 1 : 0));                 /* 000140DBCEA4: setge al */
    dispatch(c, 0x140DBCE78ull); return;                         /* 000140DBCEA7: jmp 0x140dbce78 */
}

static void bounded_switch_140DBCEA9(CPU *c)
{
    c->rip = 0x140DBCEA9ull;
    flags_sub(c, R32(c->rax), R32(c->rbx), 4);                   /* 000140DBCEA9: cmp eax, ebx */
    SET8L(c->rax, (((!c->zf && (c->sf == c->of))) ? 1 : 0));     /* 000140DBCEAB: setg al */
    dispatch(c, 0x140DBCE78ull); return;                         /* 000140DBCEAE: jmp 0x140dbce78 */
}

static void bounded_switch_140E54E92(CPU *c)
{
    c->rip = 0x140E54E92ull;
    SET32(c->rax, rd32((c->rsp + 0xB0ull)));                     /* 000140E54E92: mov eax, dword ptr [rsp + 0xb0] */
    SET64(c->rcx, (c->rsp + 0x30ull));                           /* 000140E54E99: lea rcx, [rsp + 0x30] */
    SET64(c->rdx, c->rbx);                                       /* 000140E54E9E: mov rdx, rbx */
    wr32((c->rsp + 0x20ull), (uint32_t)(R32(c->rax)));           /* 000140E54EA1: mov dword ptr [rsp + 0x20], eax */
    push64(c, 0x140E54EAAull); dispatch(c, 0x140E54010ull); c->rip = 0x140E54EAAull; /* 000140E54EA5: call 0x140e54010 */
    dispatch(c, 0x140E54F12ull); return;                         /* 000140E54EAA: jmp 0x140e54f12 */
}

static void bounded_switch_140E54EAC(CPU *c)
{
    c->rip = 0x140E54EACull;
    SET32(c->rax, rd32((c->rsp + 0xB0ull)));                     /* 000140E54EAC: mov eax, dword ptr [rsp + 0xb0] */
    SET64(c->rcx, (c->rsp + 0x38ull));                           /* 000140E54EB3: lea rcx, [rsp + 0x38] */
    SET64(c->rdx, c->rbx);                                       /* 000140E54EB8: mov rdx, rbx */
    wr32((c->rsp + 0x20ull), (uint32_t)(R32(c->rax)));           /* 000140E54EBB: mov dword ptr [rsp + 0x20], eax */
    push64(c, 0x140E54EC4ull); dispatch(c, 0x140E54550ull); c->rip = 0x140E54EC4ull; /* 000140E54EBF: call 0x140e54550 */
    dispatch(c, 0x140E54F12ull); return;                         /* 000140E54EC4: jmp 0x140e54f12 */
}

static void bounded_switch_140E54EC6(CPU *c)
{
    c->rip = 0x140E54EC6ull;
    SET32(c->rax, rd32((c->rsp + 0xB0ull)));                     /* 000140E54EC6: mov eax, dword ptr [rsp + 0xb0] */
    SET64(c->rcx, (c->rsp + 0x40ull));                           /* 000140E54ECD: lea rcx, [rsp + 0x40] */
    SET64(c->rdx, c->rbx);                                       /* 000140E54ED2: mov rdx, rbx */
    wr32((c->rsp + 0x20ull), (uint32_t)(R32(c->rax)));           /* 000140E54ED5: mov dword ptr [rsp + 0x20], eax */
    push64(c, 0x140E54EDEull); dispatch(c, 0x140E54350ull); c->rip = 0x140E54EDEull; /* 000140E54ED9: call 0x140e54350 */
    dispatch(c, 0x140E54F12ull); return;                         /* 000140E54EDE: jmp 0x140e54f12 */
}

static void bounded_switch_140E54EE0(CPU *c)
{
    c->rip = 0x140E54EE0ull;
    SET32(c->rax, rd32((c->rsp + 0xB0ull)));                     /* 000140E54EE0: mov eax, dword ptr [rsp + 0xb0] */
    SET64(c->rcx, (c->rsp + 0x48ull));                           /* 000140E54EE7: lea rcx, [rsp + 0x48] */
    SET64(c->rdx, c->rbx);                                       /* 000140E54EEC: mov rdx, rbx */
    wr32((c->rsp + 0x20ull), (uint32_t)(R32(c->rax)));           /* 000140E54EEF: mov dword ptr [rsp + 0x20], eax */
    push64(c, 0x140E54EF8ull); dispatch(c, 0x140E546F0ull); c->rip = 0x140E54EF8ull; /* 000140E54EF3: call 0x140e546f0 */
    dispatch(c, 0x140E54F12ull); return;                         /* 000140E54EF8: jmp 0x140e54f12 */
}

static void bounded_switch_140E54EFA(CPU *c)
{
    c->rip = 0x140E54EFAull;
    SET32(c->rax, rd32((c->rsp + 0xB0ull)));                     /* 000140E54EFA: mov eax, dword ptr [rsp + 0xb0] */
    SET64(c->rcx, (c->rsp + 0x50ull));                           /* 000140E54F01: lea rcx, [rsp + 0x50] */
    SET64(c->rdx, c->rbx);                                       /* 000140E54F06: mov rdx, rbx */
    wr32((c->rsp + 0x20ull), (uint32_t)(R32(c->rax)));           /* 000140E54F09: mov dword ptr [rsp + 0x20], eax */
    push64(c, 0x140E54F12ull); dispatch(c, 0x140E54F80ull); c->rip = 0x140E54F12ull; /* 000140E54F0D: call 0x140e54f80 */
    /* extent ends mid-function: fall through */
    dispatch(c, 0x140E54F12ull); return;
}

static const extra_entry entries[]={

    {UINT64_C(0x140e55b70),5,"\xe9\xfb\x05\x00\x00",ret_leaf_140E55B70},

    {UINT64_C(0x140fee310),9,"\x48\x83\xc1\x18\xe9\x37\x46\xe6\xff",ret_leaf_140FEE310},

    {UINT64_C(0x141862610),5,"\xe9\xfb\x0a\xdd\xff",ret_leaf_141862610},

    {UINT64_C(0x141a868e0),29,"\xc6\x81\xe0\x00\x00\x00\x01\x45\x33\xc0\x48\x8b\x0d\x27\x3f\x57\x02\x41\x8d\x50\x44\x48\x8b\x09\xe9\x33\x61\xd7\xff",ret_leaf_141A868E0},

    {UINT64_C(0x141ca5810),12,"\x48\x8d\x0d\x51\x40\xde\x00\xe9\x14\xa6\x23\xff",ret_leaf_141CA5810},

    {UINT64_C(0x141ca59b0),12,"\x48\x8d\x0d\xb1\x3f\xde\x00\xe9\x94\x9d\x36\xff",ret_leaf_141CA59B0},

    {UINT64_C(0x141ca5b70),12,"\x48\x8d\x0d\xe1\x44\xde\x00\xe9\xf4\x7c\x16\xff",ret_leaf_141CA5B70},

    {UINT64_C(0x141cab7b0),12,"\x48\x8d\x0d\x19\x37\x31\x02\xe9\x54\x96\x19\xff",ret_leaf_141CAB7B0},

    {UINT64_C(0x141cac080),28,"\xba\x08\x00\x00\x00\x4c\x8d\x0d\xc4\x68\x1a\xff\x48\x8d\x0d\xdd\x47\x31\x02\x44\x8d\x42\xfa\xe9\xb8\x4d\xe3\xff",ret_leaf_141CAC080},

    {UINT64_C(0x141cb9aa0),12,"\x48\x8d\x0d\xa9\x83\x33\x02\xe9\x54\x7c\x19\xff",ret_leaf_141CB9AA0},

    {UINT64_C(0x141cbac50),12,"\x48\x8d\x0d\xe1\xb2\x33\x02\xe9\xb4\xe3\x0d\xff",ret_leaf_141CBAC50},

    {UINT64_C(0x141cbdae0),12,"\x48\x8d\x0d\xf1\xcc\x33\x02\xe9\x64\x4e\x19\xff",ret_leaf_141CBDAE0},

    {UINT64_C(0x141947e84),9,"\x48\x8b\x01\xff\xa0\x08\x01\x00\x00",callback_leaf_141947E84},

    {UINT64_C(0x14108dea6),68,"\x48\x8b\x8b\xf0\x00\x00\x00\x48\x85\xc9\x74\x06\x48\x8b\x01\xff\x50\x08\x48\x89\xbb\xf0\x00\x00\x00\x48\x85\xff\x74\x08\x48\x8b\xcf\xe8\xd4\xff\xd8\xff\xf3\x0f\x7f\xb3\xb0\x00\x00\x00\x48\x8d\x4b\x20\xf3\x0f\x7f\xbb\xc0\x00\x00\x00\xe8\xbb\x08\x00\x00\xe9\xfb\xfc\xff\xff",scene_switch_14108DEA6},

    {UINT64_C(0x14108deea),294,"\x49\x8b\x80\xc0\x00\x00\x00\x41\x8b\x88\xc8\x00\x00\x00\x83\xe9\x10\x0f\x10\x08\x41\x89\x88\xc8\x00\x00\x00\x48\x8d\x50\x10\x49\x89\x90\xc0\x00\x00\x00\x83\xc1\xfc\x8b\x02\x48\x83\xc2\x04\x49\x89\x90\xc0\x00\x00\x00\x41\x89\x88\xc8\x00\x00\x00\x83\xc1\xfc\x89\x45\x28\x8b\x02\x48\x83\xc2\x04\x49\x89\x90\xc0\x00\x00\x00\x41\x89\x88\xc8\x00\x00\x00\x83\xc1\xfc\x89\x45\x38\x8b\x02\x48\x83\xc2\x04\x49\x89\x90\xc0\x00\x00\x00\x41\x89\x88\xc8\x00\x00\x00\x83\xc1\xfc\x89\x45\xd8\x8b\x02\x48\x83\xc2\x04\x49\x89\x90\xc0\x00\x00\x00\x41\x89\x88\xc8\x00\x00\x00\x83\xc1\xf8\x89\x45\xd4\x48\x8b\x3a\x48\x83\xc2\x08\x49\x89\x90\xc0\x00\x00\x00\x41\x89\x88\xc8\x00\x00\x00\x0f\x10\x32\x48\x83\xc2\x10\x49\x89\x90\xc0\x00\x00\x00\x83\xc1\xf0\x41\x89\x88\xc8\x00\x00\x00\x0f\x10\x02\x48\x8d\x42\x10\x49\x89\x80\xc0\x00\x00\x00\x8d\x41\xf0\x41\x89\x80\xc8\x00\x00\x00\xf3\x0f\x7f\x4b\x70\xf3\x0f\x7f\x4b\x60\xf3\x0f\x10\x4d\x38\x0f\x53\xf8\xc7\x83\xe8\x00\x00\x00\x02\x00\x00\x00\xf3\x0f\x10\x45\x28\xf3\x0f\x11\x8b\xd4\x00\x00\x00\xf3\x0f\x10\x4d\xd4\xf3\x0f\x11\x83\xd0\x00\x00\x00\xf3\x0f\x10\x45\xd8\xf3\x0f\x11\x8b\xe4\x00\x00\x00\xf3\x0f\x11\x83\xd8\x00\x00\x00\xe9\x96\xfe\xff\xff",scene_switch_14108DEEA},

    {UINT64_C(0x14108e010),423,"\x49\x8b\x80\xc0\x00\x00\x00\x41\x8b\x91\xc8\x00\x00\x00\x83\xea\x10\x0f\x10\x08\x41\x89\x91\xc8\x00\x00\x00\x4c\x8d\x40\x10\x4d\x89\x81\xc0\x00\x00\x00\x83\xc2\xfc\x41\x8b\x00\x49\x83\xc0\x04\x4d\x89\x81\xc0\x00\x00\x00\x41\x89\x91\xc8\x00\x00\x00\x83\xc2\xfc\x89\x45\x28\x41\x8b\x00\x49\x83\xc0\x04\x4d\x89\x81\xc0\x00\x00\x00\x41\x89\x91\xc8\x00\x00\x00\xff\xca\x89\x45\x38\x41\x0f\xb6\x08\x49\xff\xc0\x4d\x89\x81\xc0\x00\x00\x00\x41\x89\x91\xc8\x00\x00\x00\x83\xc2\xf8\x49\x8b\x38\x49\x83\xc0\x08\x4d\x89\x81\xc0\x00\x00\x00\x41\x89\x91\xc8\x00\x00\x00\x83\xc2\xf0\x41\x0f\x10\x30\x41\x89\x91\xc8\x00\x00\x00\x49\x83\xc0\x10\x4d\x89\x81\xc0\x00\x00\x00\x83\xc2\xf0\x41\x0f\x10\x00\x41\x89\x91\xc8\x00\x00\x00\x49\x83\xc0\x10\x4d\x89\x81\xc0\x00\x00\x00\x83\xc2\xfc\x0f\x53\xf8\x41\x8b\x00\x49\x83\xc0\x04\x4d\x89\x81\xc0\x00\x00\x00\x41\x89\x91\xc8\x00\x00\x00\x83\xc2\xfc\x89\x45\xd8\x41\x8b\x00\x49\x83\xc0\x04\xf3\x0f\x10\x45\x28\x4d\x89\x81\xc0\x00\x00\x00\x41\x89\x91\xc8\x00\x00\x00\x89\x45\xd0\x41\x8b\x00\x89\x45\xd4\x49\x8d\x40\x04\x49\x89\x81\xc0\x00\x00\x00\x8d\x42\xfc\x41\x89\x81\xc8\x00\x00\x00\xf3\x0f\x7f\x4b\x70\xf3\x0f\x7f\x4b\x60\xf3\x0f\x10\x4d\x38\xf3\x0f\x11\x8b\x08\x01\x00\x00\xf3\x0f\x11\x83\x04\x01\x00\x00\xc7\x83\xe8\x00\x00\x00\x04\x00\x00\x00\x88\x8b\x1a\x01\x00\x00\x48\x8b\x8b\xf0\x00\x00\x00\x48\x85\xc9\x74\x06\x48\x8b\x01\xff\x50\x08\x48\x89\xbb\xf0\x00\x00\x00\x48\x85\xff\x74\x08\x48\x8b\xcf\xe8\x25\xfd\xd8\xff\xf3\x0f\x10\x45\xd8\xf3\x0f\x10\x4d\xd4\xf3\x0f\x11\x83\x0c\x01\x00\x00\xf3\x0f\x10\x45\xd0\xf3\x0f\x11\x83\x14\x01\x00\x00\xf3\x0f\x11\x8b\x10\x01\x00\x00\xf3\x0f\x7f\xb3\xb0\x00\x00\x00\xf3\x0f\x7f\xbb\xc0\x00\x00\x00\xe9\x2e\xfa\xff\xff",scene_switch_14108E010},

    {UINT64_C(0x14108e1b7),387,"\x49\x8b\x80\xc0\x00\x00\x00\x41\x8b\x88\xc8\x00\x00\x00\x83\xe9\x10\x0f\x10\x08\x41\x89\x88\xc8\x00\x00\x00\x48\x8d\x50\x10\x49\x89\x90\xc0\x00\x00\x00\x83\xc1\xfc\x8b\x02\x48\x83\xc2\x04\x49\x89\x90\xc0\x00\x00\x00\x41\x89\x88\xc8\x00\x00\x00\x83\xc1\xfc\x89\x45\x28\x8b\x02\x48\x83\xc2\x04\x49\x89\x90\xc0\x00\x00\x00\x41\x89\x88\xc8\x00\x00\x00\x83\xc1\xfc\x89\x45\x38\x8b\x02\x48\x83\xc2\x04\x49\x89\x90\xc0\x00\x00\x00\x41\x89\x88\xc8\x00\x00\x00\x83\xc1\xfc\x89\x45\xd8\x8b\x02\x48\x83\xc2\x04\x49\x89\x90\xc0\x00\x00\x00\x41\x89\x88\xc8\x00\x00\x00\x83\xc1\xf8\x89\x45\xd4\x48\x8b\x3a\x48\x83\xc2\x08\x49\x89\x90\xc0\x00\x00\x00\x41\x89\x88\xc8\x00\x00\x00\x83\xc1\xf0\x0f\x10\x32\x41\x89\x88\xc8\x00\x00\x00\x48\x83\xc2\x10\x49\x89\x90\xc0\x00\x00\x00\x83\xc1\xf0\x0f\x10\x02\x41\x89\x88\xc8\x00\x00\x00\x48\x83\xc2\x10\x49\x89\x90\xc0\x00\x00\x00\x0f\x53\xf8\x8b\x02\x89\x45\xd0\x48\x8d\x42\x04\x49\x89\x80\xc0\x00\x00\x00\x8d\x41\xfc\x41\x89\x80\xc8\x00\x00\x00\xc7\x83\xe8\x00\x00\x00\x05\x00\x00\x00\xf3\x0f\x10\x45\x28\xf3\x0f\x7f\x4b\x70\xf3\x0f\x7f\x4b\x60\xf3\x0f\x10\x4d\x38\xf3\x0f\x11\x8b\x08\x01\x00\x00\xf3\x0f\x10\x4d\xd4\xf3\x0f\x11\x83\x04\x01\x00\x00\xf3\x0f\x10\x45\xd8\xf3\x0f\x11\x8b\x14\x01\x00\x00\xf3\x0f\x11\x83\x10\x01\x00\x00\x48\x8b\x8b\xf0\x00\x00\x00\x48\x85\xc9\x74\x06\x48\x8b\x01\xff\x50\x08\x48\x89\xbb\xf0\x00\x00\x00\x48\x85\xff\x74\x08\x48\x8b\xcf\xe8\x88\xfb\xd8\xff\xf3\x0f\x10\x45\xd0\xf3\x0f\x11\x83\x0c\x01\x00\x00\xf3\x0f\x7f\xb3\xb0\x00\x00\x00\xf3\x0f\x7f\xbb\xc0\x00\x00\x00\xe9\xab\xf8\xff\xff",scene_switch_14108E1B7},

    {UINT64_C(0x140dbd134),8,"\x0f\x2f\xc6\x0f\x93\xc0\xeb\xdb",compare_switch_140DBD134},

    {UINT64_C(0x140dbd13c),5,"\x0f\x2f\xc6\xeb\xd3",compare_switch_140DBD13C},

    {UINT64_C(0x140dbce94),7,"\x3b\xc3\x0f\x9c\xc0\xeb\xdd",bounded_switch_140DBCE94},

    {UINT64_C(0x140dbce9b),7,"\x3b\xc3\x0f\x9e\xc0\xeb\xd6",bounded_switch_140DBCE9B},

    {UINT64_C(0x140dbcea2),7,"\x3b\xc3\x0f\x9d\xc0\xeb\xcf",bounded_switch_140DBCEA2},

    {UINT64_C(0x140dbcea9),7,"\x3b\xc3\x0f\x9f\xc0\xeb\xc8",bounded_switch_140DBCEA9},

    {UINT64_C(0x140e54e92),26,"\x8b\x84\x24\xb0\x00\x00\x00\x48\x8d\x4c\x24\x30\x48\x8b\xd3\x89\x44\x24\x20\xe8\x66\xf1\xff\xff\xeb\x66",bounded_switch_140E54E92},

    {UINT64_C(0x140e54eac),26,"\x8b\x84\x24\xb0\x00\x00\x00\x48\x8d\x4c\x24\x38\x48\x8b\xd3\x89\x44\x24\x20\xe8\x8c\xf6\xff\xff\xeb\x4c",bounded_switch_140E54EAC},

    {UINT64_C(0x140e54ec6),26,"\x8b\x84\x24\xb0\x00\x00\x00\x48\x8d\x4c\x24\x40\x48\x8b\xd3\x89\x44\x24\x20\xe8\x72\xf4\xff\xff\xeb\x32",bounded_switch_140E54EC6},

    {UINT64_C(0x140e54ee0),26,"\x8b\x84\x24\xb0\x00\x00\x00\x48\x8d\x4c\x24\x48\x48\x8b\xd3\x89\x44\x24\x20\xe8\xf8\xf7\xff\xff\xeb\x18",bounded_switch_140E54EE0},

    {UINT64_C(0x140e54efa),24,"\x8b\x84\x24\xb0\x00\x00\x00\x48\x8d\x4c\x24\x50\x48\x8b\xd3\x89\x44\x24\x20\xe8\x6e\x00\x00\x00",bounded_switch_140E54EFA},

    SWITCH_EXTRA_ENTRIES

    SWITCH_EXTENSION_ENTRIES

};

#undef SWITCH_EXTRA_ENTRIES

#undef SWITCH_EXTENSION_ENTRIES
