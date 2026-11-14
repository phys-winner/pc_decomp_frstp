.code
fp_test_cmp32_flags PROC
    mov eax,ecx
    cmp eax,edx
    pushfq
    pop rax
    mov QWORD PTR [r8],rax
    ret
fp_test_cmp32_flags ENDP
END
