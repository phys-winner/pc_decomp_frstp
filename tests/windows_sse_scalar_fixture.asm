.code
fp_addss_oracle PROC
    sub rsp,18h
    stmxcsr DWORD PTR [rsp]
    mov DWORD PTR [rsp+4],r8d
    movd xmm0,ecx
    movd xmm1,edx
    ldmxcsr DWORD PTR [rsp+4]
    addss xmm0,xmm1
    movd eax,xmm0
    stmxcsr DWORD PTR [rsp+4]
    mov edx,DWORD PTR [rsp+4]
    shl rdx,32
    or rax,rdx
    ldmxcsr DWORD PTR [rsp]
    add rsp,18h
    ret
fp_addss_oracle ENDP
fp_mulss_oracle PROC
    sub rsp,18h
    stmxcsr DWORD PTR [rsp]
    mov DWORD PTR [rsp+4],r8d
    movd xmm0,ecx
    movd xmm1,edx
    ldmxcsr DWORD PTR [rsp+4]
    mulss xmm0,xmm1
    movd eax,xmm0
    stmxcsr DWORD PTR [rsp+4]
    mov edx,DWORD PTR [rsp+4]
    shl rdx,32
    or rax,rdx
    ldmxcsr DWORD PTR [rsp]
    add rsp,18h
    ret
fp_mulss_oracle ENDP
fp_rcpss_oracle PROC
    rcpss xmm0,xmm0
    ret
fp_rcpss_oracle ENDP
fp_rsqrtss_oracle PROC
    rsqrtss xmm0,xmm0
    ret
fp_rsqrtss_oracle ENDP
fp_rcpps_oracle PROC
    movups xmm0,xmmword ptr [rcx]
    rcpps xmm0,xmm0
    movups xmmword ptr [rdx],xmm0
    ret
fp_rcpps_oracle ENDP
fp_rsqrtps_oracle PROC
    movups xmm0,xmmword ptr [rcx]
    rsqrtps xmm0,xmm0
    movups xmmword ptr [rdx],xmm0
    ret
fp_rsqrtps_oracle ENDP
cvtt_native_ss32 PROC
    cvttss2si eax,xmm0
    ret
cvtt_native_ss32 ENDP
cvtt_native_ss64 PROC
    cvttss2si rax,xmm0
    ret
cvtt_native_ss64 ENDP
cvtt_native_sd32 PROC
    cvttsd2si eax,xmm0
    ret
cvtt_native_sd32 ENDP
cvtt_native_sd64 PROC
    cvttsd2si rax,xmm0
    ret
cvtt_native_sd64 ENDP
END
