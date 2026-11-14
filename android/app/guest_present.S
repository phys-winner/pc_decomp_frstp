/* Synthetic Win64 fixture, lifted to C before Android compilation. All calls
 * use guest CPU/vtable state; this is never executed as native x86 bytes. */
.intel_syntax noprefix
.text
.globl guest_create
guest_create:
    push rbx
    sub rsp, 128
    mov rbx, rcx
    xor ecx, ecx
    mov edx, 1
    xor r8d, r8d
    xor r9d, r9d
    mov rax, [rbx+8]
    mov [rsp+32], rax
    mov qword ptr [rsp+40], 1
    mov qword ptr [rsp+48], 7
    mov rax, [rbx+16]
    mov [rsp+56], rax
    mov rax, [rbx+24]
    mov [rsp+64], rax
    mov rax, [rbx+32]
    mov [rsp+72], rax
    mov rax, [rbx+40]
    mov [rsp+80], rax
    mov rax, [rbx+48]
    mov [rsp+88], rax
    call qword ptr [rbx]
    mov [rbx+96], eax
    test eax, eax
    js create_done
    mov rax, [rbx+24]
    mov rcx, [rax]
    mov rax, [rcx]
    xor edx, edx
    mov r8, [rbx+56]
    mov r9, [rbx+64]
    call qword ptr [rax+72]
    mov [rbx+104], eax
    test eax, eax
    js create_done
    mov rax, [rbx+32]
    mov rcx, [rax]
    mov rax, [rcx]
    mov rdx, [rbx+64]
    mov rdx, [rdx]
    xor r8d, r8d
    mov r9, [rbx+72]
    call qword ptr [rax+72]
    mov [rbx+112], eax
    test eax, eax
    js create_done
    mov rax, [rbx+24]
    mov rcx, [rax]
    mov rax, [rcx]
    mov rdx, [rbx+88]
    call qword ptr [rax+96]
    mov [rbx+120], eax
    test eax, eax
    js create_done
    mov qword ptr [rbx+136], 1
create_done:
    add rsp, 128
    pop rbx
    ret
.globl guest_draw
guest_draw:
    push rbx
    sub rsp, 64
    mov rbx, rcx
    mov rax, [rbx+48]
    mov rcx, [rax]
    mov rax, [rcx]
    mov rdx, [rbx+72]
    mov rdx, [rdx]
    mov r8, [rbx+80]
    call qword ptr [rax+400]
    /* Distinct corners prove compositor orientation with IDENTITY rendering. */
    .irp corner,0,1,2,3
    mov rax, [rbx+48]
    mov rcx, [rax]
    mov rax, [rcx]
    mov rdx, [rbx+64]
    mov rdx, [rdx]
    xor r8d, r8d
    mov r9, [rbx+144+8*\corner]
    mov r10, [rbx+176+8*\corner]
    mov [rsp+32], r10
    mov qword ptr [rsp+40], 512
    mov qword ptr [rsp+48], 0
    call qword ptr [rax+384]
    .endr
    mov rax, [rbx+24]
    mov rcx, [rax]
    mov rax, [rcx]
    mov edx, 1
    xor r8d, r8d
    call qword ptr [rax+64]
    mov [rbx+128], eax
    add rsp, 64
    pop rbx
    ret
