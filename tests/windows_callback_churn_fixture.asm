.code
fp_churn_call PROC FRAME
    push rbx
    .pushreg rbx
    push rbp
    .pushreg rbp
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    sub rsp,0e8h
    .allocstack 0e8h
    movdqu XMMWORD PTR [rsp+20h],xmm6
    .savexmm128 xmm6,20h
    movdqu XMMWORD PTR [rsp+30h],xmm7
    .savexmm128 xmm7,30h
    movdqu XMMWORD PTR [rsp+40h],xmm8
    .savexmm128 xmm8,40h
    movdqu XMMWORD PTR [rsp+50h],xmm9
    .savexmm128 xmm9,50h
    movdqu XMMWORD PTR [rsp+60h],xmm10
    .savexmm128 xmm10,60h
    movdqu XMMWORD PTR [rsp+70h],xmm11
    .savexmm128 xmm11,70h
    movdqu XMMWORD PTR [rsp+80h],xmm12
    .savexmm128 xmm12,80h
    movdqu XMMWORD PTR [rsp+90h],xmm13
    .savexmm128 xmm13,90h
    movdqu XMMWORD PTR [rsp+0a0h],xmm14
    .savexmm128 xmm14,0a0h
    movdqu XMMWORD PTR [rsp+0b0h],xmm15
    .savexmm128 xmm15,0b0h
    .endprolog
    mov QWORD PTR [rsp+0c0h],rcx
    mov QWORD PTR [rsp+0c8h],r8
    mov rcx,rdx
    mov ebx,11h
    mov ebp,22h
    mov esi,33h
    mov edi,44h
    mov r12d,55h
    mov r13d,66h
    mov r14d,77h
    mov r15d,88h
    pcmpeqd xmm0,xmm0
    movdqa xmm6,xmm0
    movdqa xmm7,xmm0
    movdqa xmm8,xmm0
    movdqa xmm9,xmm0
    movdqa xmm10,xmm0
    movdqa xmm11,xmm0
    movdqa xmm12,xmm0
    movdqa xmm13,xmm0
    movdqa xmm14,xmm0
    movdqa xmm15,xmm0
    call QWORD PTR [rsp+0c0h]
    mov QWORD PTR [rsp+0d0h],rax
    mov r10,QWORD PTR [rsp+0c8h]
    mov DWORD PTR [r10],0
    cmp rbx,11h
    jne failed
    cmp rbp,22h
    jne failed
    cmp rsi,33h
    jne failed
    cmp rdi,44h
    jne failed
    cmp r12,55h
    jne failed
    cmp r13,66h
    jne failed
    cmp r14,77h
    jne failed
    cmp r15,88h
    jne failed
    pcmpeqd xmm0,xmm0
    pand xmm0,xmm6
    pand xmm0,xmm7
    pand xmm0,xmm8
    pand xmm0,xmm9
    pand xmm0,xmm10
    pand xmm0,xmm11
    pand xmm0,xmm12
    pand xmm0,xmm13
    pand xmm0,xmm14
    pand xmm0,xmm15
    pcmpeqd xmm1,xmm1
    pcmpeqd xmm0,xmm1
    pmovmskb eax,xmm0
    cmp eax,0ffffh
    je restore
failed:
    mov DWORD PTR [r10],1
restore:
    movdqu xmm6,XMMWORD PTR [rsp+20h]
    movdqu xmm7,XMMWORD PTR [rsp+30h]
    movdqu xmm8,XMMWORD PTR [rsp+40h]
    movdqu xmm9,XMMWORD PTR [rsp+50h]
    movdqu xmm10,XMMWORD PTR [rsp+60h]
    movdqu xmm11,XMMWORD PTR [rsp+70h]
    movdqu xmm12,XMMWORD PTR [rsp+80h]
    movdqu xmm13,XMMWORD PTR [rsp+90h]
    movdqu xmm14,XMMWORD PTR [rsp+0a0h]
    movdqu xmm15,XMMWORD PTR [rsp+0b0h]
    mov rax,QWORD PTR [rsp+0d0h]
    add rsp,0e8h
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    ret
fp_churn_call ENDP
END
