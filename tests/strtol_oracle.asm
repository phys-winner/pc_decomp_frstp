; Native Windows oracle bridge. Executes the installed UCRT export, not game code.
OPTION CASEMAP:NONE
.code
native_strtol_rax PROC
    sub rsp, 40
    mov rax, rcx
    mov rcx, rdx
    mov rdx, r8
    mov r8, r9
    call rax
    add rsp, 40
    ret
native_strtol_rax ENDP
END
