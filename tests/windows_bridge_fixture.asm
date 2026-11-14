EXTERN fp_native_call:PROC
.const
ALIGN 16
seed BYTE 0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255
.code
; A real native entry observes its ABI inputs and returns all four result slots.
fp_test_return_registers PROC
    mov rax,123456789abcdef0h
    mov rdx,0fedcba9876543210h
    movdqu xmm0,XMMWORD PTR seed
    movdqu xmm1,XMMWORD PTR seed
    ret
fp_test_return_registers ENDP

; RCX gives the count: exercise every copied stack length, including the limit.
fp_test_stack_words PROC
    mov rax,rsp
    and eax,15
    cmp eax,8
    jne bad_alignment
    mov QWORD PTR [rsp+8],0
    mov QWORD PTR [rsp+16],0
    mov QWORD PTR [rsp+24],0
    mov QWORD PTR [rsp+32],0
    xor eax,eax
    xor r10d,r10d
words:
    cmp r10,rcx
    jae done_words
    mov r11,QWORD PTR [rsp+40+r10*8]
    inc r10
    imul r11,r10
    add rax,r11
    jmp words
done_words:
    ret
bad_alignment:
    mov ecx,5
    int 29h
fp_test_stack_words ENDP

SAVE_XMM MACRO reg,off
    movdqu XMMWORD PTR [rsp+off],reg
    .savexmm128 reg,off
ENDM
RESTORE_XMM MACRO reg,off
    movdqu reg,XMMWORD PTR [rsp+off]
ENDM
CHECK_XMM MACRO reg
    movdqu xmm0,XMMWORD PTR seed
    pcmpeqb xmm0,reg
    pmovmskb eax,xmm0
    cmp eax,0ffffh
    jne failed
ENDM
fp_test_nonvolatile PROC FRAME
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
    SAVE_XMM xmm6,40h
    SAVE_XMM xmm7,50h
    SAVE_XMM xmm8,60h
    SAVE_XMM xmm9,70h
    SAVE_XMM xmm10,80h
    SAVE_XMM xmm11,90h
    SAVE_XMM xmm12,0a0h
    SAVE_XMM xmm13,0b0h
    SAVE_XMM xmm14,0c0h
    SAVE_XMM xmm15,0d0h
    .endprolog
    mov rbx,11h
    mov rbp,22h
    mov rsi,33h
    mov rdi,44h
    mov r12,55h
    mov r13,66h
    mov r14,77h
    mov r15,88h
    movdqu xmm0,XMMWORD PTR seed
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
    call fp_native_call
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
    CHECK_XMM xmm6
    CHECK_XMM xmm7
    CHECK_XMM xmm8
    CHECK_XMM xmm9
    CHECK_XMM xmm10
    CHECK_XMM xmm11
    CHECK_XMM xmm12
    CHECK_XMM xmm13
    CHECK_XMM xmm14
    CHECK_XMM xmm15
    mov eax,1
    jmp restore
failed:
    xor eax,eax
restore:
    RESTORE_XMM xmm6,40h
    RESTORE_XMM xmm7,50h
    RESTORE_XMM xmm8,60h
    RESTORE_XMM xmm9,70h
    RESTORE_XMM xmm10,80h
    RESTORE_XMM xmm11,90h
    RESTORE_XMM xmm12,0a0h
    RESTORE_XMM xmm13,0b0h
    RESTORE_XMM xmm14,0c0h
    RESTORE_XMM xmm15,0d0h
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
fp_test_nonvolatile ENDP
END
