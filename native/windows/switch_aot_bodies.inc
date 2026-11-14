/* Exact original switch continuations selected by the 15-entry RVA table. */

static void switch_case_140EC9F6D(CPU *c)
{
    c->rip = 0x140EC9F6Dull;
    SET64(c->rax, rd64((c->rdi + 0x330ull)));                    /* 000140EC9F6D: mov rax, qword ptr [rdi + 0x330] */
    flags_logicz(c, c->rax & c->rax, 8);                         /* 000140EC9F74: test rax, rax */
    if (c->zf) { dispatch(c, 0x140ECA026ull); return; }          /* 000140EC9F77: je 0x140eca026 */
    SET64(c->rcx, rd64((c->rax)));                               /* 000140EC9F7D: mov rcx, qword ptr [rax] */
    flags_logicz(c, c->rcx & c->rcx, 8);                         /* 000140EC9F80: test rcx, rcx */
    if (c->zf) { dispatch(c, 0x140ECA026ull); return; }          /* 000140EC9F83: je 0x140eca026 */
    flags_sub(c, rd64((c->rdx)), 0x0ull, 8);                     /* 000140EC9F89: cmp qword ptr [rdx], 0 */
    if (!c->zf) { dispatch(c, 0x140ECA026ull); return; }         /* 000140EC9F8D: jne 0x140eca026 */
    push64(c, 0x140EC9F98ull); dispatch(c, 0x140EF3C00ull); c->rip = 0x140EC9F98ull; /* 000140EC9F93: call 0x140ef3c00 */
    SET64(c->rcx, rd64((c->rdi + 0x330ull)));                    /* 000140EC9F98: mov rcx, qword ptr [rdi + 0x330] */
    SET64(c->rcx, rd64((c->rcx)));                               /* 000140EC9F9F: mov rcx, qword ptr [rcx] */
    flags_sub(c, R32(c->rax), 0x2u, 4);                          /* 000140EC9FA2: cmp eax, 2 */
    if (!c->zf) goto L_000140EC9FE7;                             /* 000140EC9FA5: jne 0x140ec9fe7 */
    SET32(c->r9, 0xFFFFu);                                       /* 000140EC9FA7: mov r9d, 0xffff */
    wr32((c->rsp + 0x20ull), (uint32_t)(0xFFFFu));               /* 000140EC9FAD: mov dword ptr [rsp + 0x20], 0xffff */
    SET32(c->r8, flags_logicz(c, R32(c->r8) ^ R32(c->r8), 4));   /* 000140EC9FB5: xor r8d, r8d */
    SET8L(c->rdx, 0x1u);                                         /* 000140EC9FB8: mov dl, 1 */
    push64(c, 0x140EC9FBFull); dispatch(c, 0x140EF4D40ull); c->rip = 0x140EC9FBFull; /* 000140EC9FBA: call 0x140ef4d40 */
    SET64(c->rcx, rd64((c->rdi + 0x330ull)));                    /* 000140EC9FBF: mov rcx, qword ptr [rdi + 0x330] */
    SET32(c->r9, 0xFFFFu);                                       /* 000140EC9FC6: mov r9d, 0xffff */
    SET32(c->r8, flags_logicz(c, R32(c->r8) ^ R32(c->r8), 4));   /* 000140EC9FCC: xor r8d, r8d */
    wr32((c->rsp + 0x20ull), (uint32_t)(0xFFFFu));               /* 000140EC9FCF: mov dword ptr [rsp + 0x20], 0xffff */
    SET32(c->rdx, flags_logicz(c, R32(c->rdx) ^ R32(c->rdx), 4)); /* 000140EC9FD7: xor edx, edx */
    SET64(c->rcx, rd64((c->rcx)));                               /* 000140EC9FD9: mov rcx, qword ptr [rcx] */
    push64(c, 0x140EC9FE1ull); dispatch(c, 0x140EF4D40ull); c->rip = 0x140EC9FE1ull; /* 000140EC9FDC: call 0x140ef4d40 */
    wr32((c->rbx + 0x10ull), (uint32_t)(flags_logicz(c, rd32((c->rbx + 0x10ull)) | 0x1u, 4))); /* 000140EC9FE1: or dword ptr [rbx + 0x10], 1 */
    dispatch(c, 0x140ECA026ull); return;                         /* 000140EC9FE5: jmp 0x140eca026 */
L_000140EC9FE7:
    c->rip = 0x140EC9FE7ull;
    push64(c, 0x140EC9FECull); dispatch(c, 0x140EF3C00ull); c->rip = 0x140EC9FECull; /* 000140EC9FE7: call 0x140ef3c00 */
    flags_sub(c, R32(c->rax), 0x1u, 4);                          /* 000140EC9FEC: cmp eax, 1 */
    if (!c->zf) { dispatch(c, 0x140ECA026ull); return; }         /* 000140EC9FEF: jne 0x140eca026 */
    SET64(c->rcx, rd64((c->rdi + 0x330ull)));                    /* 000140EC9FF1: mov rcx, qword ptr [rdi + 0x330] */
    SET32(c->r9, flags_logicz(c, R32(c->r9) ^ R32(c->r9), 4));   /* 000140EC9FF8: xor r9d, r9d */
    SET32(c->r8, flags_logicz(c, R32(c->r8) ^ R32(c->r8), 4));   /* 000140EC9FFB: xor r8d, r8d */
    SET32(c->rdx, flags_logicz(c, R32(c->rdx) ^ R32(c->rdx), 4)); /* 000140EC9FFE: xor edx, edx */
    SET64(c->rcx, rd64((c->rcx)));                               /* 000140ECA000: mov rcx, qword ptr [rcx] */
    push64(c, 0x140ECA008ull); dispatch(c, 0x140EF5520ull); c->rip = 0x140ECA008ull; /* 000140ECA003: call 0x140ef5520 */
    dispatch(c, 0x140ECA026ull); return;                         /* 000140ECA008: jmp 0x140eca026 */
}

static void switch_case_140ECA00A(CPU *c)
{
    c->rip = 0x140ECA00Aull;
    SET64(c->rax, rd64((c->rdi + 0x330ull)));                    /* 000140ECA00A: mov rax, qword ptr [rdi + 0x330] */
    flags_logicz(c, c->rax & c->rax, 8);                         /* 000140ECA011: test rax, rax */
    if (c->zf) { dispatch(c, 0x140ECA026ull); return; }          /* 000140ECA014: je 0x140eca026 */
    flags_sub(c, rd64((c->rax)), 0x0ull, 8);                     /* 000140ECA016: cmp qword ptr [rax], 0 */
    if (c->zf) { dispatch(c, 0x140ECA026ull); return; }          /* 000140ECA01A: je 0x140eca026 */
    flags_sub(c, rd64((c->rdx)), 0x0ull, 8);                     /* 000140ECA01C: cmp qword ptr [rdx], 0 */
    if (!c->zf) { dispatch(c, 0x140ECA026ull); return; }         /* 000140ECA020: jne 0x140eca026 */
    wr32((c->rdx + 0x10ull), (uint32_t)(flags_logicz(c, rd32((c->rdx + 0x10ull)) | 0x1u, 4))); /* 000140ECA022: or dword ptr [rdx + 0x10], 1 */
    /* extent ends mid-function: fall through */
    dispatch(c, 0x140ECA026ull); return;
}

#define SWITCH_EXTRA_ENTRIES \
    {UINT64_C(0x140ec9f6d),157,"\x48\x8b\x87\x30\x03\x00\x00\x48\x85\xc0\x0f\x84\xa9\x00\x00\x00\x48\x8b\x08\x48\x85\xc9\x0f\x84\x9d\x00\x00\x00\x48\x83\x3a\x00\x0f\x85\x93\x00\x00\x00\xe8\x68\x9c\x02\x00\x48\x8b\x8f\x30\x03\x00\x00\x48\x8b\x09\x83\xf8\x02\x75\x40\x41\xb9\xff\xff\x00\x00\xc7\x44\x24\x20\xff\xff\x00\x00\x45\x33\xc0\xb2\x01\xe8\x81\xad\x02\x00\x48\x8b\x8f\x30\x03\x00\x00\x41\xb9\xff\xff\x00\x00\x45\x33\xc0\xc7\x44\x24\x20\xff\xff\x00\x00\x33\xd2\x48\x8b\x09\xe8\x5f\xad\x02\x00\x83\x4b\x10\x01\xeb\x3f\xe8\x14\x9c\x02\x00\x83\xf8\x01\x75\x35\x48\x8b\x8f\x30\x03\x00\x00\x45\x33\xc9\x45\x33\xc0\x33\xd2\x48\x8b\x09\xe8\x18\xb5\x02\x00\xeb\x1c",switch_case_140EC9F6D}, \
    {UINT64_C(0x140eca00a),28,"\x48\x8b\x87\x30\x03\x00\x00\x48\x85\xc0\x74\x10\x48\x83\x38\x00\x74\x0a\x48\x83\x3a\x00\x75\x04\x83\x4a\x10\x01",switch_case_140ECA00A},
