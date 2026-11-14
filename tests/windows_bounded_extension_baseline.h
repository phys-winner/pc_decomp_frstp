static void baseline_140DBCE94(CPU *c) {
    c->rip=0x140DBCE94ull;
    flags_sub(c, R32(c->rax), R32(c->rbx), 4);                   /* 000140DBCE94: cmp eax, ebx */
    SET8L(c->rax, (((c->sf != c->of)) ? 1 : 0));                 /* 000140DBCE96: setl al */
    dispatch(c, 0x140DBCE78ull); return;                         /* 000140DBCE99: jmp 0x140dbce78 */
    dispatch(c,0x140DBCE78ull);
}

static void baseline_140DBCE9B(CPU *c) {
    c->rip=0x140DBCE9Bull;
    flags_sub(c, R32(c->rax), R32(c->rbx), 4);                   /* 000140DBCE9B: cmp eax, ebx */
    SET8L(c->rax, (((c->zf || (c->sf != c->of))) ? 1 : 0));      /* 000140DBCE9D: setle al */
    dispatch(c, 0x140DBCE78ull); return;                         /* 000140DBCEA0: jmp 0x140dbce78 */
    dispatch(c,0x140DBCE78ull);
}

static void baseline_140DBCEA2(CPU *c) {
    c->rip=0x140DBCEA2ull;
    flags_sub(c, R32(c->rax), R32(c->rbx), 4);                   /* 000140DBCEA2: cmp eax, ebx */
    SET8L(c->rax, (((c->sf == c->of)) ? 1 : 0));                 /* 000140DBCEA4: setge al */
    dispatch(c, 0x140DBCE78ull); return;                         /* 000140DBCEA7: jmp 0x140dbce78 */
    dispatch(c,0x140DBCE78ull);
}

static void baseline_140DBCEA9(CPU *c) {
    c->rip=0x140DBCEA9ull;
    flags_sub(c, R32(c->rax), R32(c->rbx), 4);                   /* 000140DBCEA9: cmp eax, ebx */
    SET8L(c->rax, (((!c->zf && (c->sf == c->of))) ? 1 : 0));     /* 000140DBCEAB: setg al */
    dispatch(c, 0x140DBCE78ull); return;                         /* 000140DBCEAE: jmp 0x140dbce78 */
    dispatch(c,0x140DBCE78ull);
}

static void baseline_140E54E92(CPU *c) {
    c->rip=0x140E54E92ull;
    SET32(c->rax, rd32((c->rsp + 0xB0ull)));                     /* 000140E54E92: mov eax, dword ptr [rsp + 0xb0] */
    SET64(c->rcx, (c->rsp + 0x30ull));                           /* 000140E54E99: lea rcx, [rsp + 0x30] */
    SET64(c->rdx, c->rbx);                                       /* 000140E54E9E: mov rdx, rbx */
    wr32((c->rsp + 0x20ull), (uint32_t)(R32(c->rax)));           /* 000140E54EA1: mov dword ptr [rsp + 0x20], eax */
    push64(c, 0x140E54EAAull); dispatch(c, 0x140E54010ull); c->rip = 0x140E54EAAull; /* 000140E54EA5: call 0x140e54010 */
    { dispatch(c,0x000140E54F12ull); return; }                                         /* 000140E54EAA: jmp 0x140e54f12 */
    dispatch(c,0x140E54F12ull);
}

static void baseline_140E54EAC(CPU *c) {
    c->rip=0x140E54EACull;
    SET32(c->rax, rd32((c->rsp + 0xB0ull)));                     /* 000140E54EAC: mov eax, dword ptr [rsp + 0xb0] */
    SET64(c->rcx, (c->rsp + 0x38ull));                           /* 000140E54EB3: lea rcx, [rsp + 0x38] */
    SET64(c->rdx, c->rbx);                                       /* 000140E54EB8: mov rdx, rbx */
    wr32((c->rsp + 0x20ull), (uint32_t)(R32(c->rax)));           /* 000140E54EBB: mov dword ptr [rsp + 0x20], eax */
    push64(c, 0x140E54EC4ull); dispatch(c, 0x140E54550ull); c->rip = 0x140E54EC4ull; /* 000140E54EBF: call 0x140e54550 */
    { dispatch(c,0x000140E54F12ull); return; }                                         /* 000140E54EC4: jmp 0x140e54f12 */
    dispatch(c,0x140E54F12ull);
}

static void baseline_140E54EC6(CPU *c) {
    c->rip=0x140E54EC6ull;
    SET32(c->rax, rd32((c->rsp + 0xB0ull)));                     /* 000140E54EC6: mov eax, dword ptr [rsp + 0xb0] */
    SET64(c->rcx, (c->rsp + 0x40ull));                           /* 000140E54ECD: lea rcx, [rsp + 0x40] */
    SET64(c->rdx, c->rbx);                                       /* 000140E54ED2: mov rdx, rbx */
    wr32((c->rsp + 0x20ull), (uint32_t)(R32(c->rax)));           /* 000140E54ED5: mov dword ptr [rsp + 0x20], eax */
    push64(c, 0x140E54EDEull); dispatch(c, 0x140E54350ull); c->rip = 0x140E54EDEull; /* 000140E54ED9: call 0x140e54350 */
    { dispatch(c,0x000140E54F12ull); return; }                                         /* 000140E54EDE: jmp 0x140e54f12 */
    dispatch(c,0x140E54F12ull);
}

static void baseline_140E54EE0(CPU *c) {
    c->rip=0x140E54EE0ull;
    SET32(c->rax, rd32((c->rsp + 0xB0ull)));                     /* 000140E54EE0: mov eax, dword ptr [rsp + 0xb0] */
    SET64(c->rcx, (c->rsp + 0x48ull));                           /* 000140E54EE7: lea rcx, [rsp + 0x48] */
    SET64(c->rdx, c->rbx);                                       /* 000140E54EEC: mov rdx, rbx */
    wr32((c->rsp + 0x20ull), (uint32_t)(R32(c->rax)));           /* 000140E54EEF: mov dword ptr [rsp + 0x20], eax */
    push64(c, 0x140E54EF8ull); dispatch(c, 0x140E546F0ull); c->rip = 0x140E54EF8ull; /* 000140E54EF3: call 0x140e546f0 */
    { dispatch(c,0x000140E54F12ull); return; }                                         /* 000140E54EF8: jmp 0x140e54f12 */
    dispatch(c,0x140E54F12ull);
}

static void baseline_140E54EFA(CPU *c) {
    c->rip=0x140E54EFAull;
    SET32(c->rax, rd32((c->rsp + 0xB0ull)));                     /* 000140E54EFA: mov eax, dword ptr [rsp + 0xb0] */
    SET64(c->rcx, (c->rsp + 0x50ull));                           /* 000140E54F01: lea rcx, [rsp + 0x50] */
    SET64(c->rdx, c->rbx);                                       /* 000140E54F06: mov rdx, rbx */
    wr32((c->rsp + 0x20ull), (uint32_t)(R32(c->rax)));           /* 000140E54F09: mov dword ptr [rsp + 0x20], eax */
    push64(c, 0x140E54F12ull); dispatch(c, 0x140E54F80ull); c->rip = 0x140E54F12ull; /* 000140E54F0D: call 0x140e54f80 */
    dispatch(c,0x140E54F12ull);
}
