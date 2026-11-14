; Static Win64 ABI adapters. CPU offsets are checked by the C fixture.
.code
fp_xgetbv PROC
    xgetbv
    shl rdx,32
    or rax,rdx
    ret
fp_xgetbv ENDP

fp_qsort_float_difference PROC
    stmxcsr DWORD PTR [rsp+8]
    movd xmm0,ecx
    movd xmm1,edx
    subss xmm0,xmm1
    cvttss2si eax,xmm0
    ldmxcsr DWORD PTR [rsp+8]
    ret
fp_qsort_float_difference ENDP

fp_native_call PROC FRAME
    push rbx
    .pushreg rbx
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r12
    .pushreg r12
    sub rsp,128h
    .allocstack 128h
    .endprolog
    cmp r8d,256
    ja invalid_native_frame
    test r8d,7
    jnz invalid_native_frame
    mov rbx,rcx
    mov r12,rdx
    mov rsi,QWORD PTR [rbx+20h]
    mov QWORD PTR [rsp+120h],rsi
    add rsi,40
    lea rdi,[rsp+20h]
    mov ecx,r8d
    cld
    cmp ecx,32
    jb copy_tail16
copy_loop32:
    movdqu xmm4,XMMWORD PTR [rsi]
    movdqu xmm5,XMMWORD PTR [rsi+16]
    movdqu XMMWORD PTR [rdi],xmm4
    movdqu XMMWORD PTR [rdi+16],xmm5
    add rsi,32
    add rdi,32
    sub ecx,32
    cmp ecx,32
    jae copy_loop32
copy_tail16:
    test ecx,16
    jz copy_tail8
    movdqu xmm4,XMMWORD PTR [rsi]
    movdqu XMMWORD PTR [rdi],xmm4
    add rsi,16
    add rdi,16
copy_tail8:
    test ecx,8
    jz copy_done
    mov rax,QWORD PTR [rsi]
    mov QWORD PTR [rdi],rax
copy_done:
    movdqu xmm0,XMMWORD PTR [rbx+0b8h]
    movdqu xmm1,XMMWORD PTR [rbx+0c8h]
    movdqu xmm2,XMMWORD PTR [rbx+0d8h]
    movdqu xmm3,XMMWORD PTR [rbx+0e8h]
    mov rcx,QWORD PTR [rbx+8]
    mov rdx,QWORD PTR [rbx+10h]
    mov r8,QWORD PTR [rbx+40h]
    mov r9,QWORD PTR [rbx+48h]
    call r12
    mov QWORD PTR [rbx],rax
    mov QWORD PTR [rbx+10h],rdx
    movdqu XMMWORD PTR [rbx+0b8h],xmm0
    movdqu XMMWORD PTR [rbx+0c8h],xmm1
    mov rax,QWORD PTR [rsp+120h]
    mov rcx,QWORD PTR [rax]
    mov QWORD PTR [rbx+80h],rcx
    add rax,8
    mov QWORD PTR [rbx+20h],rax
    add rsp,128h
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret
invalid_native_frame:
    mov ecx,5
    int 29h
fp_native_call ENDP

fp_on_alt_stack PROC FRAME
    push rbx
    .pushreg rbx
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
    sub rsp,28h
    .allocstack 28h
    .endprolog
    mov rdi,rcx
    and rdi,-16
    cmp rdi,rdx
    jbe invalid_alt_stack
    mov rax,rdi
    sub rax,rdx
    cmp rax,64
    jb invalid_alt_stack
    mov r12,rsp
    mov r13,QWORD PTR gs:[8]
    mov r14,QWORD PTR gs:[10h]
    mov rbx,r8
    mov rsi,r9
    mov QWORD PTR gs:[8],rdi
    mov QWORD PTR gs:[10h],rdx
    lea rsp,[rdi-20h]
    mov rcx,rsi
    call rbx
    mov rsp,r12
    mov QWORD PTR gs:[8],r13
    mov QWORD PTR gs:[10h],r14
    add rsp,28h
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret
invalid_alt_stack:
    mov ecx,5
    int 29h
fp_on_alt_stack ENDP
END
