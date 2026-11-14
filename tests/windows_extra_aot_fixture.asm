.code
fp_test_add24 PROC
    mov rax,rcx
    add rax,24
    pushfq
    pop rcx
    mov QWORD PTR [rdx],rcx
    ret
fp_test_add24 ENDP
END
