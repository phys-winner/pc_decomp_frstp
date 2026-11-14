option casemap:none
.const
ALIGN 16
compare_exponent DWORD 4 DUP(07f800000h)
compare_fraction DWORD 4 DUP(007fffffh)
.code

; These instructions are compiled host source; no game instructions execute.
fp_simd_float_mxcsr_mask_native PROC FRAME
    sub rsp,208h
    .allocstack 208h
    .endprolog
    mov dword ptr [rsp+1ch],0
    fxsave [rsp]
    mov eax,dword ptr [rsp+1ch]
    test eax,eax
    jnz mask_ready
    mov eax,0ffbfh
mask_ready:
    add rsp,208h
    ret
fp_simd_float_mxcsr_mask_native ENDP

; uint32_t fp_cvttpd2dq_native(XMM *out,const XMM *source,uint32_t mxcsr)
fp_cvttpd2dq_native PROC FRAME
    sub rsp,18h
    .allocstack 18h
    .endprolog
    movdqu xmm0,xmmword ptr [rdx]
    stmxcsr dword ptr [rsp]
    mov dword ptr [rsp+4],r8d
    ldmxcsr dword ptr [rsp+4]
    cvttpd2dq xmm0,xmm0
    stmxcsr dword ptr [rsp+4]
    ldmxcsr dword ptr [rsp]
    movdqu xmmword ptr [rcx],xmm0
    mov eax,dword ptr [rsp+4]
    add rsp,18h
    ret
fp_cvttpd2dq_native ENDP

; uint32_t fp_haddps_native(XMM *out,const XMM *d,const XMM *s,uint32_t mxcsr)
fp_haddps_native PROC FRAME
    sub rsp,18h
    .allocstack 18h
    .endprolog
    movdqu xmm0,xmmword ptr [rdx]
    movdqu xmm1,xmmword ptr [r8]
    stmxcsr dword ptr [rsp]
    mov dword ptr [rsp+4],r9d
    ldmxcsr dword ptr [rsp+4]
    haddps xmm0,xmm1
    stmxcsr dword ptr [rsp+4]
    ldmxcsr dword ptr [rsp]
    movdqu xmmword ptr [rcx],xmm0
    mov eax,dword ptr [rsp+4]
    add rsp,18h
    ret
fp_haddps_native ENDP

; Internal helpers: fp_simd_packed_compare validates MXCSR bits/masks before every call.
PACKED_COMPARE MACRO function_name, instruction_name, predicate
LOCAL full_environment
function_name PROC FRAME
    sub rsp,18h
    .allocstack 18h
    .endprolog
    movdqu xmm0,xmmword ptr [rdx]
    movdqu xmm1,xmmword ptr [r8]
    IFIDNI <instruction_name>, <cmpps>
    ; No NaN/subnormal in either operand: CMPPS cannot change MXCSR.
    movdqa xmm2,xmm0
    movdqa xmm3,xmm1
    pand xmm2,xmmword ptr [compare_exponent]
    pand xmm3,xmmword ptr [compare_exponent]
    pxor xmm4,xmm4
    pcmpeqd xmm4,xmm2
    pcmpeqd xmm2,xmmword ptr [compare_exponent]
    por xmm2,xmm4
    pxor xmm5,xmm5
    pcmpeqd xmm5,xmm3
    pcmpeqd xmm3,xmmword ptr [compare_exponent]
    por xmm3,xmm5
    movdqa xmm4,xmm0
    movdqa xmm5,xmm1
    pand xmm4,xmmword ptr [compare_fraction]
    pand xmm5,xmmword ptr [compare_fraction]
    pand xmm2,xmm4
    pand xmm3,xmm5
    por xmm2,xmm3
    pxor xmm4,xmm4
    pcmpeqd xmm2,xmm4
    pmovmskb eax,xmm2
    cmp eax,0ffffh
    jne full_environment
    instruction_name xmm0,xmm1,predicate
    movdqu xmmword ptr [rcx],xmm0
    mov eax,r9d
    add rsp,18h
    ret
full_environment:
    ENDIF
    stmxcsr dword ptr [rsp]
    mov dword ptr [rsp+4],r9d
    ldmxcsr dword ptr [rsp+4]
    instruction_name xmm0,xmm1,predicate
    stmxcsr dword ptr [rsp+4]
    ldmxcsr dword ptr [rsp]
    movdqu xmmword ptr [rcx],xmm0
    mov eax,dword ptr [rsp+4]
    add rsp,18h
    ret
function_name ENDP
ENDM
PACKED_COMPARE fp_cmpeqps_native, cmpps, 0
PACKED_COMPARE fp_cmpltps_native, cmpps, 1
PACKED_COMPARE fp_cmpeqpd_native, cmppd, 0
PACKED_COMPARE fp_cmpltpd_native, cmppd, 1
PACKED_COMPARE fp_cmpnlepd_native, cmppd, 6

END
