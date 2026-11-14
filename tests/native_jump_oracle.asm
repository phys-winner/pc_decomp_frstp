.const
ALIGN 16
seed_xmm BYTE 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
 BYTE 16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
 BYTE 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
 BYTE 48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
 BYTE 64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79
 BYTE 80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95
 BYTE 96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111
 BYTE 112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
 BYTE 128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143
 BYTE 144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159
.code
jump_read_cw PROC
 sub rsp,8
 fnstcw WORD PTR [rsp]
 movzx eax,WORD PTR [rsp]
 add rsp,8
 ret
jump_read_cw ENDP
jump_write_cw PROC
 sub rsp,8
 mov WORD PTR [rsp],cx
 fldcw WORD PTR [rsp]
 add rsp,8
 ret
jump_write_cw ENDP
; void jump_capture(function, buffer, frame, observation*).
jump_capture PROC FRAME
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
 sub rsp,0f8h
 .allocstack 0f8h
 movdqu XMMWORD PTR [rsp+50h],xmm6
 .savexmm128 xmm6,50h
 movdqu XMMWORD PTR [rsp+60h],xmm7
 .savexmm128 xmm7,60h
 movdqu XMMWORD PTR [rsp+70h],xmm8
 .savexmm128 xmm8,70h
 movdqu XMMWORD PTR [rsp+80h],xmm9
 .savexmm128 xmm9,80h
 movdqu XMMWORD PTR [rsp+90h],xmm10
 .savexmm128 xmm10,90h
 movdqu XMMWORD PTR [rsp+0a0h],xmm11
 .savexmm128 xmm11,0a0h
 movdqu XMMWORD PTR [rsp+0b0h],xmm12
 .savexmm128 xmm12,0b0h
 movdqu XMMWORD PTR [rsp+0c0h],xmm13
 .savexmm128 xmm13,0c0h
 movdqu XMMWORD PTR [rsp+0d0h],xmm14
 .savexmm128 xmm14,0d0h
 movdqu XMMWORD PTR [rsp+0e0h],xmm15
 .savexmm128 xmm15,0e0h
 .endprolog
 mov QWORD PTR [rsp+20h],rcx
 mov QWORD PTR [rsp+28h],rdx
 mov QWORD PTR [rsp+30h],r8
 mov QWORD PTR [rsp+38h],r9
 stmxcsr DWORD PTR [rsp+40h]
 fnstcw WORD PTR [rsp+44h]
 ldmxcsr DWORD PTR [r9+40]
 fldcw WORD PTR [r9+44]
 mov QWORD PTR [r9],rsp
 lea rax,jump_return
 mov QWORD PTR [r9+8],rax
 mov rbx,1111111122222201h
 mov rbp,1111111122222202h
 mov rsi,1111111122222203h
 mov rdi,1111111122222204h
 mov r12,1111111122222205h
 mov r13,1111111122222206h
 mov r14,1111111122222207h
 mov r15,1111111122222208h
 movdqu xmm6,XMMWORD PTR seed_xmm
 movdqu xmm7,XMMWORD PTR seed_xmm+16
 movdqu xmm8,XMMWORD PTR seed_xmm+32
 movdqu xmm9,XMMWORD PTR seed_xmm+48
 movdqu xmm10,XMMWORD PTR seed_xmm+64
 movdqu xmm11,XMMWORD PTR seed_xmm+80
 movdqu xmm12,XMMWORD PTR seed_xmm+96
 movdqu xmm13,XMMWORD PTR seed_xmm+112
 movdqu xmm14,XMMWORD PTR seed_xmm+128
 movdqu xmm15,XMMWORD PTR seed_xmm+144
 mov r11,QWORD PTR [rsp+20h]
 mov rcx,QWORD PTR [rsp+28h]
 mov rdx,QWORD PTR [rsp+30h]
 call r11
jump_return:
 mov r10,QWORD PTR [rsp+38h]
 mov QWORD PTR [r10+24],rax
 mov QWORD PTR [r10+16],rsp
 mov QWORD PTR [r10+56],rbx
 mov QWORD PTR [r10+64],rbp
 mov QWORD PTR [r10+72],rsi
 mov QWORD PTR [r10+80],rdi
 mov QWORD PTR [r10+88],r12
 mov QWORD PTR [r10+96],r13
 mov QWORD PTR [r10+104],r14
 mov QWORD PTR [r10+112],r15
 stmxcsr DWORD PTR [r10+48]
 fnstcw WORD PTR [r10+52]
 movdqu XMMWORD PTR [r10+120],xmm6
 movdqu XMMWORD PTR [r10+136],xmm7
 movdqu XMMWORD PTR [r10+152],xmm8
 movdqu XMMWORD PTR [r10+168],xmm9
 movdqu XMMWORD PTR [r10+184],xmm10
 movdqu XMMWORD PTR [r10+200],xmm11
 movdqu XMMWORD PTR [r10+216],xmm12
 movdqu XMMWORD PTR [r10+232],xmm13
 movdqu XMMWORD PTR [r10+248],xmm14
 movdqu XMMWORD PTR [r10+264],xmm15
 ldmxcsr DWORD PTR [rsp+40h]
 fldcw WORD PTR [rsp+44h]
 movdqu xmm6,XMMWORD PTR [rsp+50h]
 movdqu xmm7,XMMWORD PTR [rsp+60h]
 movdqu xmm8,XMMWORD PTR [rsp+70h]
 movdqu xmm9,XMMWORD PTR [rsp+80h]
 movdqu xmm10,XMMWORD PTR [rsp+90h]
 movdqu xmm11,XMMWORD PTR [rsp+0a0h]
 movdqu xmm12,XMMWORD PTR [rsp+0b0h]
 movdqu xmm13,XMMWORD PTR [rsp+0c0h]
 movdqu xmm14,XMMWORD PTR [rsp+0d0h]
 movdqu xmm15,XMMWORD PTR [rsp+0e0h]
 add rsp,0f8h
 pop r15
 pop r14
 pop r13
 pop r12
 pop rdi
 pop rsi
 pop rbp
 pop rbx
 ret
jump_capture ENDP
END
