/* Manually scheduled static recompilation of original 1410596D0..70F
 * and 141059810..84F. No cache, allocation, custom wide read or skipped variant.
 * Pure byte loads use locals; the final iteration materializes exactly the
 * original CPU state before its match/append boundary, including AF and the
 * first-iteration dirty high half of R9. No guest memory is written here. */
#include "shader_lookup_aot.h"
#include <string.h>
#include <errno.h>

static int shader_lookup_equal(uint64_t left,uint64_t right)
{
    for(;;){
        size_t n=4096u-(left&4095u),right_span=4096u-(right&4095u);
        if(n>right_span)n=right_span;
        if(n>64u)n=64u;
        /* Keep libc reads inside both mapped pages, even without a terminator. */
        if(n<16u){
            unsigned a=rd8(left),b=rd8(right);
            if(a!=b)return 0;
            if(!b)return 1;
            ++left;++right;
        }else{
            if(strncmp((const char*)(uintptr_t)left,(const char*)(uintptr_t)right,n))return 0;
            if(memchr((const void*)(uintptr_t)right,0,n))return 1;
            left+=n;right+=n;
        }
    }
}

static int shader_lookup_scan(CPU *c,uint64_t outer_rip,uint64_t byte_rip,uint64_t compare_rip)
{
    const uint64_t records=c->r12,key=c->rbp;
    const int32_t limit=(int32_t)R32(c->r11);
    uint64_t index=c->r9,address=0;
    uint32_t left=0,right=0;
    int saved_errno=errno;
    c->rip=byte_rip;
    for(;;){
        uint64_t offset=((uint64_t)(int64_t)(int32_t)(uint32_t)index*9u)<<7;
        address=records+offset;
        uint32_t next=(uint32_t)index+1u;
        /* Replay the selected candidate to recover the exact original state. */
        if((int32_t)next>=limit || shader_lookup_equal(address,key))break;
        index=next;
    }
    errno=saved_errno;
    uint64_t key_address=key;
    for(;;){
        left=rd8(address);right=rd8(key_address);
        if(left!=right || right==0)break;
        ++address;++key_address;
    }
    c->r9=index;c->rip=outer_rip;
    SET64(c->rax,(uint64_t)(int64_t)(int32_t)R32(c->r9));
    SET64(c->r8,c->rbp);
    SET64(c->r10,c->rax+c->rax*8);
    SET64(c->r10,op_shl(c,c->r10,7u,8));
    SET64(c->rdi,c->r12+c->r10);
    SET64(c->rax,address);
    SET64(c->r8,flags_sub(c,c->r8,c->rdi,8));
    c->rip=byte_rip;
    SET32(c->rdx,left);SET32(c->rcx,right);
    SET32(c->rdx,flags_sub(c,R32(c->rdx),R32(c->rcx),4));
    if(!R32(c->rdx)){
        SET64(c->rax,flags_incs(c,c->rax,8));
        flags_logicz(c,R32(c->rcx),4);
    }
    c->rip=compare_rip;
    flags_logicz(c,R32(c->rdx),4);
    if(c->zf)return 1;
    SET32(c->r9,flags_incs(c,R32(c->r9),4));
    flags_sub(c,R32(c->r9),R32(c->r11),4);
    return 0;
}

void frostpunk_shader_lookup_1410596a9(CPU *c)
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
    if(shader_lookup_scan(c,0x1410596D0ull,0x1410596F0ull,0x141059703ull)){dispatch(c,0x141059750ull);return;}
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

void frostpunk_shader_lookup_1410597e9(CPU *c)
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
    if(shader_lookup_scan(c,0x141059810ull,0x141059830ull,0x141059843ull)){dispatch(c,0x141059890ull);return;}
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
