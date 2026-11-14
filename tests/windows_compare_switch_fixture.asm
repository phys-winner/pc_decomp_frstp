.code
fp_test_comiss_flags PROC
    movups xmm0,XMMWORD PTR [rcx]
    movups xmm1,XMMWORD PTR [rdx]
    comiss xmm0,xmm1
    pushfq
    pop rax
    mov QWORD PTR [r8],rax
    ret
fp_test_comiss_flags ENDP
END
